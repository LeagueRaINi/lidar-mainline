// SPDX-License-Identifier: GPL-2.0-only
// lidar-assistant: the voice assistant's core. It takes what was said as
// text, one utterance per line, finds the action it asks for with the local
// grammar (grammar.cc), carries it out and answers with a sentence
// (speech.cc). What the grammar does not understand is reported as
// "unmatched", for a language model to handle with the same actions.
//
// Usage: lidar-assistant [options]
//   -l en|de    language (default en); its file is <dir>/<language>.txt
//   -L <dir>    language files (default /usr/share/lidar/assistant)
//   -d <dir>    where alarms are kept (default /var/lib/lidar-assistant)
//   -r <dir>    runtime files (default /run): lidar-assistant.fifo (input),
//               lidar-assistant.json (state), lidar-volume.state and
//               lidar-time.synced (read)
//   -y <cmd>    program that speaks an answer: <cmd> <language> <text>, the
//               parts of the text in LIDAR_SAY_PARTS, separated by "|"
//               (default: print "say <language> <text>")
//   -w <file>   WAV file an alarm or timer rings with (default:
//               <data dir>/alarm.wav if there is one, else a chime)
//   -o <pcm>    ALSA device it rings on (default lidar_speaker)
//   -m <0-100>  volume it rings at, at least, in percent; the speaker is
//               unmuted for it (default 30; 0: volume and mute stay)
//   -b <dev>    input device whose buttons stop the ringing (default: the
//               one named gpio-keys; none: no buttons)
//   -t          test: utterances from stdin instead of the FIFO, commands
//               printed ("run ...") instead of run, a clock that only moves
//               by "!advance <seconds>" and is unset until "!clock
//               <YYYY-MM-DD> <HH:MM>"; "!state" prints the state
//   -n          with -t: only print the action of each utterance
//   -P          print every phrase and number word of the answers, and exit
//   -S          print the actions as JSON (name, description, input_schema),
//               and exit
//   -T <sec>    write the chime to stdout (48 kHz S16 stereo), and exit
//
// Input (the FIFO): one utterance per line. "!stop" stops the ringing and
// nothing else; that is what a button sends.
//
// Output, one line each: "action <name> <arguments>", then "unmatched
// <text>" or "result <JSON>" and, unless -y, "say <language> <text>".
//
// Timers run on the time since boot and are gone after a reboot. Alarms are
// wall clock times kept in <dir>/alarms; they are neither set nor do they
// ring before lidar-time has the time from the network (lidar-time.synced).
// An alarm or timer rings until it is stopped ("stop", a button, "!stop"),
// for 10 minutes at most.
#include "assistant.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static const int MAX_TIMERS = 10, MAX_ALARMS = 20;
static const long RING_MAX = 600;	// seconds of ringing before it gives up
static const long RING_WINDOW = 600;	// an alarm missed by more is not rung late

static Lang lang = EN;
static const char *const LANG_NAMES[] = { "en", "de" };
static bool test_mode, parse_only;
static std::string lang_dir = "/usr/share/lidar/assistant";
static std::string data_dir = "/var/lib/lidar-assistant", run_dir = "/run";
static bool data_dir_given;
static std::string say_command, sound_file, speaker = "lidar_speaker", button = "auto";
static int ring_volume = 30;

struct Timer {
	std::string name;
	long seconds, ends;	// ends: seconds since boot
};

struct Alarm {
	int id, hour, minute;
	int repeat;	// weekdays, bit 0 Monday; 0: once, at
	time_t at;
	time_t last;	// repeating: when it last rang, or was set
};

struct Ringing {
	bool timer;
	std::string name;
	int hour, minute;
};

static std::vector<Timer> timers;
static std::vector<Alarm> alarms;
static std::vector<Ringing> ringing;
static long ring_since;
static pid_t ring_pid;
static int volume_before = -1;	// percent to go back to after ringing
static int volume_ringing;	// percent the ringing set
static bool muted_before;
static bool dirty = true;	// the state file is out of date
static bool clock_was_ok;

// Clocks ----------------------------------------------------------------------

static long test_mono;
static time_t test_wall;	// wall clock at test_mono 0; 0: not set

static long mono_now()
{
	struct timespec ts;

	if (test_mode)
		return test_mono;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec;
}

static time_t wall_now()
{
	return test_mode ? test_wall + test_mono : time(nullptr);
}

// The board has no real-time clock: the time is right once lidar-time got
// it from the network
static bool clock_ok()
{
	if (test_mode)
		return test_wall != 0;
	return !access((run_dir + "/lidar-time.synced").c_str(), F_OK);
}

// hour:minute local time on the day of t, or days later
static time_t at_local(time_t t, int days, int hour, int minute)
{
	struct tm tm;

	localtime_r(&t, &tm);
	tm.tm_mday += days;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static int weekday_of(time_t t)	// 0 Monday
{
	struct tm tm;

	localtime_r(&t, &tm);
	return (tm.tm_wday + 6) % 7;
}

// When the alarm rings next. A repeating one: the first of its times that
// it has not rung at and that is not long past.
static time_t next_due(const Alarm &a, time_t now)
{
	if (!a.repeat)
		return a.at;
	for (int d = 0; d < 400; d++) {
		time_t t = at_local(now, d, a.hour, a.minute);

		if ((a.repeat & 1 << weekday_of(t)) && t > a.last && t + RING_WINDOW > now)
			return t;
	}
	return now + 400 * 86400;	// the clock went far back since it rang
}

// Commands ---------------------------------------------------------------------

// Runs a command and waits for it, 5 s at most: lidar-volume blocks on its
// FIFO if its daemon died. Returns its exit status, -1 if it did not run.
static int run(const std::vector<std::string> &argv, int timeout = 5)
{
	std::vector<char *> args;
	int status;

	for (const std::string &arg : argv)
		args.push_back(const_cast<char *>(arg.c_str()));
	args.push_back(nullptr);
	if (test_mode) {
		printf("run");
		for (const std::string &arg : argv)
			printf(" %s", arg.c_str());
		printf("\n");
		return 0;
	}
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		signal(SIGPIPE, SIG_DFL);
		prctl(PR_SET_PDEATHSIG, SIGTERM);	// not left behind
		alarm((unsigned)timeout);
		execvp(args[0], args.data());
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// lidar-volume's state: "level muted vmax". The volume in percent of vmax,
// as lidar-volume get says it.
static bool read_volume(int *percent, bool *muted, int *max = nullptr)
{
	FILE *f = fopen((run_dir + "/lidar-volume.state").c_str(), "r");
	int level, mute, vmax, n;

	if (!f)
		return false;
	n = fscanf(f, "%d %d %d", &level, &mute, &vmax);
	fclose(f);
	if (n != 3 || vmax <= 0)
		return false;
	*percent = (level * 100 + vmax / 2) / vmax;
	if (max)
		*max = vmax;
	*muted = mute != 0;
	return true;
}

// Ringing ---------------------------------------------------------------------

// The chime: two notes a second (G5, C6), each struck and dying away,
// starting quiet (-36 dBFS) and rising to -6 dBFS over 30 s
static void chime(int16_t *out, int frames, long frame)
{
	for (int i = 0; i < frames; i++, frame++) {
		double t = (double)frame / 48000, beat = fmod(t, 1.0);
		double freq = beat < 0.2 ? 784 : 1047, at = beat < 0.2 ? beat : beat - 0.2;
		double length = beat < 0.2 ? 0.2 : 0.4, s = 0;

		if (beat < 0.6) {
			double envelope = fmin(1, at / 0.008) * exp(-at / 0.18) *
					  fmin(1, (length - at) / 0.01);
			double db = -36 + fmin(t, 30.0);

			s = 32767 * pow(10, db / 20) * envelope * sin(2 * M_PI * freq * at);
		}
		out[2 * i] = out[2 * i + 1] = (int16_t)lrint(s);
	}
}

// The sound display keeps off our own sounds: until the time, in seconds
// since boot, in /run/lidar-viz.quiet (as lidar-volume and lidar-bluetooth
// write it). The later end wins; the file is replaced, never half written.
static void viz_quiet(double seconds)
{
	struct timespec t;
	double until, old = 0;
	std::string path = run_dir + "/lidar-viz.quiet", tmp = path + "." + std::to_string(getpid());

	clock_gettime(CLOCK_BOOTTIME, &t);
	until = t.tv_sec + t.tv_nsec / 1e9 + seconds;
	if (FILE *f = fopen(path.c_str(), "r")) {
		if (fscanf(f, "%lf", &old) != 1)
			old = 0;
		fclose(f);
	}
	if (old >= until)
		return;
	if (FILE *f = fopen(tmp.c_str(), "w")) {
		fprintf(f, "%.2f\n", until);
		if (fclose(f) == 0 && rename(tmp.c_str(), path.c_str()) == 0)
			return;
	}
	unlink(tmp.c_str());
}

// For seconds, or until the reader goes away. live: the sound display keeps
// off meanwhile (1 s ahead: the speaker tuning and the buffers lag)
static void write_chime(int fd, double seconds, bool live)
{
	int16_t block[2 * 4800];

	for (long frame = 0; frame < seconds * 48000; frame += 4800) {
		chime(block, 4800, frame);
		if (live)
			viz_quiet(1.0);
		for (size_t done = 0; done < sizeof(block);) {
			ssize_t n = write(fd, (char *)block + done, sizeof(block) - done);

			if (n <= 0)
				return;
			done += (size_t)n;
		}
	}
}

// The player is a process group of its own: the user's WAV file over and
// over, or the chime (also if the file does not play) into aplay, like the
// other sounds through the speaker's dmix. Killed as a group to stop; it
// also ends with the daemon, and by itself when the ringing would.
static void start_player()
{
	ring_pid = fork();
	if (ring_pid > 0)
		setpgid(ring_pid, ring_pid);
	if (ring_pid)
		return;
	setpgid(0, 0);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() == 1)	// the daemon died meanwhile
		_exit(0);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	std::string file = sound_file.empty() ? data_dir + "/alarm.wav" : sound_file;
	long end = mono_now() + RING_MAX;
	struct stat st;
	// a 48 kHz 16-bit stereo WAV: its length, and 0.3 s for the lag
	while (mono_now() < end && !stat(file.c_str(), &st) &&
	       (viz_quiet((st.st_size - 44) / 192000.0 + 0.3),
		!run({ "aplay", "-q", "-D", speaker, file }, (int)(end - mono_now()))))
		usleep(500000);
	if (mono_now() >= end)
		_exit(0);

	int fd[2];
	if (pipe(fd))
		_exit(1);
	pid_t pid = fork();
	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		dup2(fd[0], 0);
		close(fd[0]);
		close(fd[1]);
		execlp("aplay", "aplay", "-q", "-D", speaker.c_str(), "-t", "raw", "-f", "S16_LE",
		       "-c", "2", "-r", "48000", (char *)nullptr);
		_exit(127);
	}
	close(fd[0]);
	write_chime(fd[1], (double)(end - mono_now()), true);
	_exit(1);
}

static void stop_player()
{
	if (ring_pid <= 0)
		return;
	kill(-ring_pid, SIGTERM);
	while (waitpid(ring_pid, nullptr, 0) < 0 && errno == EINTR)
		;
	ring_pid = 0;
}

static void ring(const Ringing &r)
{
	int percent, vmax;
	bool muted;

	ringing.push_back(r);
	dirty = true;
	if (ringing.size() > 1)
		return;
	ring_since = mono_now();
	// audible whatever the volume was left at
	volume_before = -1;
	muted_before = false;
	if (ring_volume && read_volume(&percent, &muted, &vmax)) {
		if (muted) {
			muted_before = true;
			run({ "lidar-volume", "unmute" });
		}
		if (percent < ring_volume) {
			volume_before = percent;
			// what lidar-volume makes of it: a codec level, as percent
			volume_ringing = ((ring_volume * vmax + 50) / 100 * 100 + vmax / 2) / vmax;
			run({ "lidar-volume", "set", std::to_string(ring_volume) });
		}
	}
	if (test_mode)
		printf("ring start\n");
	else
		start_player();
}

static bool stop_ringing()
{
	int percent;
	bool muted;

	if (ringing.empty())
		return false;
	ringing.clear();
	dirty = true;
	if (test_mode)
		printf("ring stop\n");
	else
		stop_player();
	// back to the volume from before, unless it was changed meanwhile
	if (volume_before >= 0 && read_volume(&percent, &muted) && percent == volume_ringing)
		run({ "lidar-volume", "set", std::to_string(volume_before) });
	if (muted_before)
		run({ "lidar-volume", "mute" });
	return true;
}

// State -----------------------------------------------------------------------

static std::string json(const std::string &s)
{
	std::string out = "\"";
	char hex[8];

	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c < 0x20) {
			snprintf(hex, sizeof(hex), "\\u%04x", c);
			out += hex;
		} else {
			out += (char)c;
		}
	}
	return out + "\"";
}

static std::string days_json(int mask)
{
	std::string s;

	for (int d = 0; d < 7; d++)
		if (mask & 1 << d)
			s += (s.empty() ? "" : ",") + json(DAY_NAMES[d]);
	return "[" + s + "]";
}

// The last utterance and what became of it, for the web interface
static struct {
	long seq, uptime;
	std::string text, action, say;
	bool ok;
} last;

// Valid UTF-8 of what came in: the state file has to stay JSON
static std::string utf8(const std::string &s)
{
	std::string out;

	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		size_t len = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 :
			     c >= 0xF0 && c < 0xF5 ? 4 : 0, k = 1;

		while (k < len && i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80)
			k++;
		if (len && k == len) {
			out.append(s, i, len);
			i += len;
		} else {
			out += '?';
			i++;
		}
	}
	return out;
}

// For the web interface and the LED ring. Timers end at "ends" seconds
// since boot (/proc/uptime); "remaining" is as of "uptime", when the file
// was written. It is written when something changes, not every second.
// "last": the last utterance (seq counts them), null before the first.
static std::string state()
{
	long mono = mono_now();
	time_t now = wall_now();
	bool ok = clock_ok();
	std::string s = "{\"language\":" + json(LANG_NAMES[lang]) + ",\"clock\":" +
			(ok ? "true" : "false") + ",\"uptime\":" + std::to_string(mono) +
			",\"ringing\":[";

	for (size_t i = 0; i < ringing.size(); i++) {
		const Ringing &r = ringing[i];

		s += i ? "," : "";
		if (r.timer)
			s += "{\"kind\":\"timer\",\"name\":" + json(r.name) + "}";
		else
			s += "{\"kind\":\"alarm\",\"hour\":" + std::to_string(r.hour) +
			     ",\"minute\":" + std::to_string(r.minute) + "}";
	}
	s += "],\"timers\":[";
	for (size_t i = 0; i < timers.size(); i++) {
		const Timer &t = timers[i];

		s += std::string(i ? "," : "") + "{\"name\":" + json(t.name) + ",\"seconds\":" +
		     std::to_string(t.seconds) + ",\"ends\":" + std::to_string(t.ends) +
		     ",\"remaining\":" + std::to_string(std::max(0L, t.ends - mono)) + "}";
	}
	s += "],\"alarms\":[";
	for (size_t i = 0; i < alarms.size(); i++) {
		const Alarm &a = alarms[i];

		s += std::string(i ? "," : "") + "{\"id\":" + std::to_string(a.id) + ",\"hour\":" +
		     std::to_string(a.hour) + ",\"minute\":" + std::to_string(a.minute) +
		     ",\"repeat\":" + days_json(a.repeat) + ",\"next\":" +
		     std::to_string(ok || !a.repeat ? (long long)next_due(a, now) : 0) + "}";
	}
	s += "],\"last\":";
	if (!last.seq)
		return s + "null}";
	return s + "{\"seq\":" + std::to_string(last.seq) + ",\"uptime\":" +
	       std::to_string(last.uptime) + ",\"text\":" + json(last.text) + ",\"action\":" +
	       json(last.action) + ",\"ok\":" + (last.ok ? "true" : "false") + ",\"say\":" +
	       json(last.say) + "}}";
}

// sync: on disk before it replaces the file (the alarms; not for /run)
static bool write_file(const std::string &path, const std::string &content, bool sync = false)
{
	std::string tmp = path + ".new";
	FILE *f = fopen(tmp.c_str(), "w");

	if (!f)
		return false;
	bool ok = fputs(content.c_str(), f) >= 0 && !fflush(f) && (!sync || !fsync(fileno(f)));
	ok = !fclose(f) && ok;
	return ok && !rename(tmp.c_str(), path.c_str());
}

static void write_state()
{
	dirty = false;
	if (!test_mode)
		write_file(run_dir + "/lidar-assistant.json", state() + "\n");
}

// <dir>/alarms: "id hour minute repeat at last" per alarm
static void save_alarms()
{
	std::string s;

	for (const Alarm &a : alarms)
		s += std::to_string(a.id) + " " + std::to_string(a.hour) + " " +
		     std::to_string(a.minute) + " " + std::to_string(a.repeat) + " " +
		     std::to_string((long long)a.at) + " " + std::to_string((long long)a.last) + "\n";
	dirty = true;
	if (test_mode && !data_dir_given)	// a test never touches the real alarms
		return;
	mkdir(data_dir.c_str(), 0755);
	if (!write_file(data_dir + "/alarms", s, true))
		fprintf(stderr, "lidar-assistant: %s/alarms: %s\n", data_dir.c_str(), strerror(errno));
}

static void load_alarms()
{
	FILE *f = test_mode && !data_dir_given ? nullptr : fopen((data_dir + "/alarms").c_str(), "r");
	long long at, last;
	Alarm a;

	if (!f)
		return;
	while (fscanf(f, "%d %d %d %d %lld %lld", &a.id, &a.hour, &a.minute, &a.repeat, &at,
		      &last) == 6) {
		a.at = (time_t)at;
		a.last = (time_t)last;
		if (a.hour >= 0 && a.hour < 24 && a.minute >= 0 && a.minute < 60 && a.repeat >= 0 &&
		    a.repeat < 128 && (int)alarms.size() < MAX_ALARMS)
			alarms.push_back(a);
	}
	fclose(f);
}

// Actions ----------------------------------------------------------------------

struct Result {
	bool ok = true;
	const char *code = "ok";
	Reply say;	// empty: nothing to say
};

static Reply reply()
{
	Reply r;

	r.lang = lang;
	return r;
}

static Result done(Reply &r, const char *code = "ok")
{
	Result res;

	res.code = code;
	res.say = r.end();
	return res;
}

static Result silent(const char *code)
{
	Result res;

	res.code = code;
	return res;
}

static Result fail(const char *code, Phrase phrase)
{
	Result res;

	res.ok = false;
	res.code = code;
	res.say = reply().p(phrase).end();
	return res;
}

static std::string arg(const Action &a, const char *name)
{
	auto it = a.args.find(name);

	return it == a.args.end() ? "" : it->second;
}

static long number(const Action &a, const char *name, long otherwise = 0)
{
	auto it = a.args.find(name);

	return it == a.args.end() ? otherwise : atol(it->second.c_str());
}

static std::string lower(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return s;
}

static Result volume_set(const Action &a)
{
	int level = (int)number(a, "level");

	if (run({ "lidar-volume", "set", std::to_string(level * 10) }))
		return fail("no_volume", P_NO_VOLUME);
	return done(reply().p(P_VOLUME).number(level));
}

static Result volume_change(const Action &a)
{
	bool up = arg(a, "direction") == "up";

	if (run({ "lidar-volume", up ? "up" : "down" }))
		return fail("no_volume", P_NO_VOLUME);
	return done(reply().p(up ? P_LOUDER : P_QUIETER));
}

static Result volume_get(const Action &)
{
	int percent;
	bool muted;

	if (!read_volume(&percent, &muted))
		return fail("no_volume", P_NO_VOLUME);
	Reply r = reply().p(P_VOLUME).number((percent + 5) / 10);
	if (muted)
		r.end().p(P_MUTED);
	return done(r);
}

static Result volume_mute(const Action &a)
{
	bool mute = arg(a, "mute") == "on";

	if (run({ "lidar-volume", mute ? "mute" : "unmute" }))
		return fail("no_volume", P_NO_VOLUME);
	return done(reply().p(mute ? P_MUTED : P_UNMUTED));
}

static Result media(const Action &a)
{
	std::string command = arg(a, "command");
	Phrase phrase = command == "pause" ? P_PAUSED : command == "resume" ? P_RESUMED :
			command == "next" ? P_NEXT : P_PREVIOUS;

	if (run({ "lidar-bluetooth", "media", command == "resume" ? "play" : command }))
		return fail("failed", P_FAILED);
	return done(reply().p(phrase));
}

// Whatever makes noise: the ringing, or else what plays. Nothing is said.
static Result stop(const Action &)
{
	if (stop_ringing())
		return silent("ring_stopped");
	run({ "lidar-bluetooth", "media", "pause" });
	return silent("paused");
}

static std::vector<Timer>::iterator find_timer(const std::string &name)
{
	return std::find_if(timers.begin(), timers.end(),
			    [&](const Timer &t) { return lower(t.name) == lower(name); });
}

static Result timer_set(const Action &a)
{
	Timer t;

	// "|" separates the parts of an answer for the say command
	for (unsigned char c : arg(a, "name"))
		if (c >= 0x20 && c != 0x7F && c != '|')
			t.name += (char)c;
	t.seconds = number(a, "seconds");
	t.ends = mono_now() + t.seconds;
	auto same = find_timer(t.name);
	if (!t.name.empty() && same != timers.end())
		timers.erase(same);	// set again
	if ((int)timers.size() >= MAX_TIMERS)
		return fail("too_many", P_TOO_MANY);
	timers.push_back(t);
	dirty = true;
	return done(reply().word(t.name).p(P_TIMER_SET).duration(t.seconds).p(P_TIMER_SET_END));
}

static Result timer_cancel(const Action &a)
{
	std::string name = arg(a, "name");
	bool all = number(a, "all");
	auto t = timers.end();

	// "stop the timer" while one rings
	if (!all && name.empty() && stop_ringing())
		return silent("ring_stopped");
	if (timers.empty())
		return fail("no_timers", P_NO_TIMERS);
	if (all) {
		timers.clear();
		dirty = true;
		return done(reply().p(P_TIMERS_CANCELLED));
	}
	if (!name.empty()) {
		if ((t = find_timer(name)) == timers.end())
			return fail("no_such_timer", P_NO_SUCH_TIMER);
	} else if (timers.size() == 1) {
		t = timers.begin();
	} else {	// the one without a name, if there is just one
		for (auto i = timers.begin(); i != timers.end(); i++) {
			if (!i->name.empty())
				continue;
			if (t != timers.end())
				return fail("several_timers", P_SEVERAL_TIMERS);
			t = i;
		}
		if (t == timers.end())
			return fail("several_timers", P_SEVERAL_TIMERS);
	}
	Reply r = reply().word(t->name).p(P_TIMER_CANCELLED);
	timers.erase(t);
	dirty = true;
	return done(r);
}

static Result timer_list(const Action &a)
{
	std::string name = arg(a, "name");
	std::vector<Timer> list = timers;
	long mono = mono_now();
	Reply r = reply();

	if (timers.empty())
		return fail("no_timers", P_NO_TIMERS);
	if (!name.empty()) {
		auto t = find_timer(name);

		if (t == timers.end())
			return fail("no_such_timer", P_NO_SUCH_TIMER);
		list = { *t };
	}
	std::sort(list.begin(), list.end(),
		  [](const Timer &x, const Timer &y) { return x.ends < y.ends; });
	for (const Timer &t : list)
		r.word(t.name).p(P_TIMER).p(P_LEFT).duration(std::max(0L, t.ends - mono))
			.p(P_LEFT_END).end();
	return done(r);
}

static int days_mask(const std::string &list)
{
	int mask = 0;

	for (int d = 0; d < 7; d++)
		if (("," + list + ",").find(std::string(",") + DAY_NAMES[d] + ",") != std::string::npos)
			mask |= 1 << d;
	return mask;
}

// "seven thirty AM tomorrow", "... every Monday and Friday"
static Reply &alarm_phrase(Reply &r, const Alarm &a, time_t now)
{
	r.clock(a.hour, a.minute);
	if (a.repeat == 127)
		return r.p(P_EVERY_DAY);
	if (a.repeat == 31)
		return r.p(P_WEEKDAYS);
	if (a.repeat == 96)
		return r.p(P_WEEKENDS);
	if (a.repeat) {
		int left = __builtin_popcount((unsigned)a.repeat);

		r.p(P_EVERY);
		for (int d = 0; d < 7; d++) {
			if (!(a.repeat & 1 << d))
				continue;
			r.weekday(d);
			if (--left == 1)
				r.p(P_AND);
		}
		return r;
	}
	if (a.at < at_local(now, 1, 0, 0))
		return r.p(P_TODAY);
	if (a.at < at_local(now, 2, 0, 0))
		return r.p(P_TOMORROW);
	return r.p(P_ON_DAY).weekday(weekday_of(a.at));
}

static Result alarm_set(const Action &a)
{
	std::string day = arg(a, "day");
	time_t now = wall_now();
	Alarm al = {};

	if (!clock_ok())
		return fail("no_clock", P_NO_CLOCK);
	al.hour = (int)number(a, "hour");
	al.minute = (int)number(a, "minute");
	al.repeat = days_mask(arg(a, "repeat"));
	al.last = now;
	if (!al.repeat) {
		// "at seven" without a day: whichever seven comes first
		std::vector<int> hours = { al.hour };
		if (number(a, "h12") && al.hour >= 1 && al.hour <= 12 && (day.empty() || day == "today"))
			hours = { al.hour % 12, al.hour % 12 + 12 };
		for (int hour : hours) {
			time_t t = 0;

			if (day.empty()) {
				t = at_local(now, 0, hour, al.minute);
				if (t <= now)
					t = at_local(now, 1, hour, al.minute);
			} else if (day == "today" || day == "tomorrow") {
				t = at_local(now, day == "tomorrow", hour, al.minute);
			} else {	// the next such weekday
				for (int d = 0; d <= 7 && !t; d++) {
					time_t on = at_local(now, d, hour, al.minute);

					if (on > now && DAY_NAMES[weekday_of(on)] == day)
						t = on;
				}
			}
			if (t > now && (!al.at || t < al.at)) {
				al.at = t;
				al.hour = hour;
			}
		}
		if (!al.at)
			return fail("time_past", P_TIME_PAST);
	}
	bool known = false;
	for (const Alarm &other : alarms) {
		known = known || (other.hour == al.hour && other.minute == al.minute &&
				  other.repeat == al.repeat && other.at == al.at);
		al.id = std::max(al.id, other.id);
	}
	al.id++;
	if (!known) {
		if ((int)alarms.size() >= MAX_ALARMS)
			return fail("too_many", P_TOO_MANY);
		alarms.push_back(al);
		save_alarms();
	}
	Reply r = reply().p(P_ALARM_SET);
	return done(alarm_phrase(r, al, now));
}

static bool sooner(const Alarm &x, const Alarm &y)
{
	time_t now = wall_now();

	return next_due(x, now) < next_due(y, now);
}

static Result alarm_cancel(const Action &a)
{
	bool all = number(a, "all"), timed = a.args.count("hour");
	int hour = (int)number(a, "hour"), minute = (int)number(a, "minute");
	bool h12 = number(a, "h12");
	time_t now = wall_now();
	Reply r = reply();

	// "stop the alarm" while one rings
	if (!all && !timed && stop_ringing())
		return silent("ring_stopped");
	if (alarms.empty())
		return fail("no_alarms", P_NO_ALARMS);
	if (all) {
		alarms.clear();
		save_alarms();
		return done(r.p(P_ALARMS_CANCELLED));
	}
	// without a time it can only mean the one alarm there is
	if (!timed && alarms.size() > 1)
		return fail("several_alarms", P_SEVERAL_ALARMS);
	for (auto i = alarms.begin(); i != alarms.end();) {
		bool match = !timed || (i->minute == minute &&
					(h12 ? i->hour % 12 == hour % 12 : i->hour == hour));

		if (!match) {
			i++;
			continue;
		}
		alarm_phrase(r.p(P_ALARM_CANCELLED), *i, now).end();
		i = alarms.erase(i);
	}
	if (r.parts.empty())
		return fail("no_such_alarm", P_NO_SUCH_ALARM);
	save_alarms();
	return done(r);
}

static Result alarm_list(const Action &)
{
	time_t now = wall_now();
	Reply r = reply();

	if (alarms.empty())
		return fail("no_alarms", P_NO_ALARMS);
	std::sort(alarms.begin(), alarms.end(), sooner);
	for (const Alarm &a : alarms)
		alarm_phrase(r.p(P_ALARM_AT), a, now).end();
	return done(r);
}

static Result time_get(const Action &)
{
	time_t now = wall_now();
	struct tm tm;

	if (!clock_ok())
		return fail("no_clock", P_NO_CLOCK);
	localtime_r(&now, &tm);
	return done(reply().p(P_TIME).clock(tm.tm_hour, tm.tm_min));
}

static Result date_get(const Action &)
{
	time_t now = wall_now();
	struct tm tm;

	if (!clock_ok())
		return fail("no_clock", P_NO_CLOCK);
	localtime_r(&now, &tm);
	return done(reply().date((tm.tm_wday + 6) % 7, tm.tm_mday, tm.tm_mon));
}

// The actions: what the grammar's commands are carried out with and, later,
// the tools a language model is given (-S prints them for that). Arguments
// are checked against this table before an action runs.
enum Type {
	INT,	// min to max
	ENUM,	// one of values ("a|b")
	TEXT,	// up to max bytes
	DAYS,	// weekdays "mon,wed"; a JSON array of them in the schema
};

struct Param {
	const char *name;
	Type type;
	long min, max;
	const char *values;
	bool required;
	const char *help;
};

struct Tool {
	const char *name, *help;
	std::vector<Param> params;
	Result (*run)(const Action &);
};

static const char WEEK[] = "mon|tue|wed|thu|fri|sat|sun";

static const Tool TOOLS[] = {
	{ "volume_set", "Set the speaker volume.",
	  { { "level", INT, 0, 10, nullptr, true, "0 (silent) to 10 (loudest)" } }, volume_set },
	{ "volume_change", "Turn the speaker volume up or down one step (3 dB).",
	  { { "direction", ENUM, 0, 0, "up|down", true, "" } }, volume_change },
	{ "volume_get", "Say the speaker volume (0 to 10) and whether it is muted.", {}, volume_get },
	{ "volume_mute", "Mute or unmute the speaker. The microphones cannot be changed.",
	  { { "mute", ENUM, 0, 0, "on|off", true, "on: muted" } }, volume_mute },
	{ "stop", "Stop a ringing alarm or timer; if nothing rings, pause the music.", {}, stop },
	{ "media", "Control the music a phone plays over Bluetooth, and the song library.",
	  { { "command", ENUM, 0, 0, "pause|resume|next|previous", true, "" } }, media },
	{ "timer_set", "Start a timer. A timer with the same name is replaced.",
	  { { "seconds", INT, 1, 86400, nullptr, true, "duration" },
	    { "name", TEXT, 0, 40, nullptr, false, "what it is for, e.g. pizza" } }, timer_set },
	{ "timer_cancel", "Cancel a timer: the named one, all, or the only one. Stops a ringing "
			  "timer or alarm if neither name nor all is given.",
	  { { "name", TEXT, 0, 40, nullptr, false, "" },
	    { "all", INT, 0, 1, nullptr, false, "1: every timer" } }, timer_cancel },
	{ "timer_list", "Say how long the timers have left: the named one, or all.",
	  { { "name", TEXT, 0, 40, nullptr, false, "" } }, timer_list },
	{ "alarm_set", "Set an alarm for a time of day, local time: once (the next time it is "
		       "that time, or on day), or repeating on weekdays.",
	  { { "hour", INT, 0, 23, nullptr, true, "" },
	    { "minute", INT, 0, 59, nullptr, false, "default 0" },
	    { "h12", INT, 0, 1, nullptr, false,
	      "1: hour (1 to 12) was said without AM or PM; the next of both is taken" },
	    { "day", ENUM, 0, 0, "today|tomorrow|mon|tue|wed|thu|fri|sat|sun", false,
	      "once, on this day (a weekday: the next one)" },
	    { "repeat", DAYS, 0, 0, WEEK, false, "every week on these days" } }, alarm_set },
	{ "alarm_cancel", "Cancel alarms: those at a time, all, or the only one. Stops a ringing "
			  "alarm or timer if neither time nor all is given.",
	  { { "hour", INT, 0, 23, nullptr, false, "" },
	    { "minute", INT, 0, 59, nullptr, false, "default 0" },
	    { "h12", INT, 0, 1, nullptr, false, "1: hour may be AM or PM" },
	    { "all", INT, 0, 1, nullptr, false, "1: every alarm" } }, alarm_cancel },
	{ "alarm_list", "Say the alarms that are set.", {}, alarm_list },
	{ "time_get", "Say the time.", {}, time_get },
	{ "date_get", "Say today's weekday and date.", {}, date_get },
};

static bool one_of(const std::string &value, const char *values)
{
	return !value.empty() && value.find('|') == std::string::npos &&
	       ("|" + std::string(values) + "|").find("|" + value + "|") != std::string::npos;
}

static bool valid(const Tool &tool, const Action &a)
{
	for (const auto &given : a.args) {
		const Param *p = nullptr;
		const std::string &v = given.second;

		for (const Param &param : tool.params)
			if (given.first == param.name)
				p = &param;
		if (!p)
			return false;
		switch (p->type) {
		case INT:
			if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos ||
			    atol(v.c_str()) < p->min || atol(v.c_str()) > p->max)
				return false;
			break;
		case ENUM:
			if (!one_of(v, p->values))
				return false;
			break;
		case TEXT:
			if ((long)v.size() > p->max)
				return false;
			break;
		case DAYS:
			for (size_t from = 0, to; from <= v.size(); from = to + 1) {
				to = std::min(v.find(',', from), v.size());
				if (!one_of(v.substr(from, to - from), p->values))
					return false;
			}
			break;
		}
	}
	for (const Param &param : tool.params)
		if (param.required && !a.args.count(param.name))
			return false;
	return true;
}

static Result execute(const Action &a)
{
	for (const Tool &tool : TOOLS)
		if (a.name == tool.name)
			return valid(tool, a) ? tool.run(a) : fail("bad_arguments", P_BAD_ARGS);
	return fail("no_such_action", P_BAD_ARGS);
}

// The actions as a language model's tool list
static void print_tools()
{
	printf("[\n");
	for (const Tool &tool : TOOLS) {
		std::string required;

		printf("%s{\"name\":%s,\"description\":%s,\"input_schema\":{\"type\":\"object\","
		       "\"properties\":{", &tool == TOOLS ? "" : ",\n", json(tool.name).c_str(),
		       json(tool.help).c_str());
		for (const Param &p : tool.params) {
			std::string values;

			for (const char *v = p.values; v && *v;) {
				size_t len = strcspn(v, "|");

				values += (values.empty() ? "" : ",") + json(std::string(v, len));
				v += len + (v[len] ? 1 : 0);
			}
			printf("%s%s:{", &p == &tool.params[0] ? "" : ",", json(p.name).c_str());
			if (p.type == INT)
				printf("\"type\":\"integer\",\"minimum\":%ld,\"maximum\":%ld", p.min, p.max);
			else if (p.type == ENUM)
				printf("\"type\":\"string\",\"enum\":[%s]", values.c_str());
			else if (p.type == TEXT)
				printf("\"type\":\"string\",\"maxLength\":%ld", p.max);
			else
				printf("\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[%s]}",
				       values.c_str());
			if (*p.help)
				printf(",\"description\":%s", json(p.help).c_str());
			printf("}");
			if (p.required)
				required += (required.empty() ? "" : ",") + json(p.name);
		}
		printf("},\"required\":[%s]}}", required.c_str());
	}
	printf("\n]\n");
}

// Input -----------------------------------------------------------------------

static void speak(const Reply &r)
{
	std::string text = r.text(), parts;

	if (text.empty())
		return;
	if (say_command.empty()) {
		printf("say %s %s\n", LANG_NAMES[lang], text.c_str());
		return;
	}
	for (const std::string &part : r.parts)
		parts += (parts.empty() ? "" : "|") + part;
	// not waited for: a child that leaves its own child behind
	pid_t pid = fork();
	if (!pid) {
		if (fork())
			_exit(0);
		setenv("LIDAR_SAY_PARTS", parts.c_str(), 1);
		execlp(say_command.c_str(), say_command.c_str(), LANG_NAMES[lang], text.c_str(),
		       (char *)nullptr);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
			;
}

// Timers and alarms that are due, and the ringing
static void tick()
{
	long mono = mono_now();
	bool ok = clock_ok();

	for (auto t = timers.begin(); t != timers.end();) {
		if (t->ends > mono) {
			t++;
			continue;
		}
		ring({ true, t->name, 0, 0 });
		t = timers.erase(t);
	}
	if (ok != clock_was_ok)
		dirty = true;
	clock_was_ok = ok;
	if (ok) {
		time_t now = wall_now();
		bool changed = false;

		for (auto a = alarms.begin(); a != alarms.end();) {
			time_t due = next_due(*a, now);

			if (now < due) {
				a++;
				continue;
			}
			changed = true;
			if (now - due <= RING_WINDOW)	// else: it was off at that time
				ring({ false, "", a->hour, a->minute });
			a->last = now;
			a = a->repeat ? a + 1 : alarms.erase(a);
		}
		if (changed)
			save_alarms();
	}
	if (!ringing.empty()) {
		if (mono - ring_since >= RING_MAX)
			stop_ringing();
		else if (!test_mode && waitpid(ring_pid, nullptr, WNOHANG) != 0)
			start_player();	// it ended (no sound device just now?)
	}
	if (dirty)
		write_state();
}

// Carries the action out, reports the result and speaks the answer
static Result perform(const Action &a)
{
	Result res = execute(a);
	std::string args;

	for (const auto &given : a.args)
		args += (args.empty() ? "" : ",") + json(given.first) + ":" + json(given.second);
	printf("result {\"action\":%s,\"arguments\":{%s},\"ok\":%s,\"code\":%s,\"say\":%s}\n",
	       json(a.name).c_str(), args.c_str(), res.ok ? "true" : "false",
	       json(res.code).c_str(), json(res.say.text()).c_str());
	speak(res.say);
	return res;
}

static void handle(const std::string &line)
{
	if (line.empty())
		return;
	if (test_mode && !parse_only)
		printf("> %s\n", line.c_str());
	if (line == "!stop") {
		stop_ringing();
	} else if (test_mode && !line.compare(0, 9, "!advance ")) {
		// in steps, as time passes
		for (long left = atol(line.c_str() + 9); left > 0; left -= 30) {
			test_mono += std::min(left, 30L);
			tick();
		}
	} else if (test_mode && !line.compare(0, 7, "!clock ")) {
		struct tm tm = {};

		if (sscanf(line.c_str() + 7, "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
			   &tm.tm_hour, &tm.tm_min) == 5) {
			tm.tm_year -= 1900;
			tm.tm_mon--;
			tm.tm_isdst = -1;
			test_wall = mktime(&tm) - test_mono;
		}
	} else if (test_mode && line == "!state") {
		printf("state %s\n", state().c_str());
	} else if (test_mode && !line.compare(0, 8, "!action ")) {
		// an action as a language model would ask for it
		Action a;
		char *save, *text = strdup(line.c_str() + 8);

		for (char *word = strtok_r(text, " ", &save); word; word = strtok_r(nullptr, " ", &save)) {
			char *eq = strchr(word, '=');

			if (a.name.empty())
				a.name = word;
			else if (eq)
				a.args[std::string(word, (size_t)(eq - word))] = eq + 1;
		}
		free(text);
		perform(a);
	} else if (line[0] != '!') {
		Action a = parse(lang, line);

		printf("action %s\n", a.str().c_str());
		if (parse_only)
			return;
		Result res;
		if (a.name == "unmatched") {
			printf("unmatched %s\n", line.c_str());
			res.ok = false;
		} else {
			res = perform(a);
		}
		last.seq++;
		last.uptime = mono_now();
		last.text = utf8(line);
		last.action = a.name;
		last.ok = res.ok;
		last.say = res.say.text();
		dirty = true;
	}
	tick();
}

// The buttons' input device, by name as lidar-volume finds it
static int open_buttons()
{
	char line[256], path[64] = "";
	bool found = false;

	if (button == "none")
		return -1;
	if (button != "auto")
		return open(button.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	FILE *f = fopen("/proc/bus/input/devices", "r");
	while (f && fgets(line, sizeof(line), f)) {
		const char *event = strstr(line, "event");

		if (!strncmp(line, "N: ", 3))
			found = strstr(line, "\"gpio-keys\"") != nullptr;
		if (found && !strncmp(line, "H: ", 3) && event) {
			snprintf(path, sizeof(path), "/dev/input/event%d", atoi(event + 5));
			break;
		}
	}
	if (f)
		fclose(f);
	return *path ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1;
}

// struct input_event of the kernel on 64-bit systems (linux/input.h, which
// the sysroot the tools are built against does not have)
struct InputEvent {
	int64_t sec, usec;
	uint16_t type, code;
	int32_t value;
};

static volatile sig_atomic_t quit;

static void on_signal(int)
{
	quit = 1;
}

// Seconds until something is due, in ms for poll(); -1: nothing
static int timeout()
{
	long mono = mono_now(), wait = -1;
	auto within = [&](long s) { wait = wait < 0 || s < wait ? std::max(s, 0L) : wait; };

	for (const Timer &t : timers)
		within(t.ends - mono);
	if (!ringing.empty())
		within(2);
	if (!clock_ok())	// the state file says when the clock is there
		within(5);
	else if (!alarms.empty())	// alarms are due on the minute
		within(60 - (long)(wall_now() % 60));
	return wait < 0 ? -1 : (int)(wait * 1000 + 20);
}

int main(int argc, char **argv)
{
	bool phrases = false, tools = false;
	double chime_seconds = 0;
	std::string error;
	int c;

	while ((c = getopt(argc, argv, "l:L:d:r:y:w:o:m:b:tnPST:")) != -1) {
		switch (c) {
		case 'l':
			if (strcmp(optarg, "en") && strcmp(optarg, "de")) {
				fprintf(stderr, "lidar-assistant: -l en|de\n");
				return 2;
			}
			lang = optarg[0] == 'd' ? DE : EN;
			break;
		case 'L': lang_dir = optarg; break;
		case 'd':
			data_dir = optarg;
			data_dir_given = true;
			break;
		case 'r': run_dir = optarg; break;
		case 'y': say_command = optarg; break;
		case 'w': sound_file = optarg; break;
		case 'o': speaker = optarg; break;
		case 'm': ring_volume = std::max(0, std::min(100, atoi(optarg))); break;
		case 'b': button = optarg; break;
		case 't': test_mode = true; break;
		case 'n': parse_only = true; break;
		case 'P': phrases = true; break;
		case 'S': tools = true; break;
		case 'T': chime_seconds = atof(optarg); break;
		default:
			fprintf(stderr, "usage: lidar-assistant [-l en|de] [-L <language dir>] [-d <data dir>] "
					"[-r <run dir>] [-y <say command>] [-w <wav>] [-o <pcm>] "
					"[-m <percent>] [-b <input device>|none] [-t [-n]] [-P] [-S] "
					"[-T <seconds>]\n");
			return 2;
		}
	}
	if (tools) {
		print_tools();
		return 0;
	}
	if (chime_seconds > 0) {
		write_chime(1, chime_seconds, false);
		return 0;
	}
	if (!load_language(lang_dir + "/" + LANG_NAMES[lang] + ".txt", &error)) {
		fprintf(stderr, "lidar-assistant: %s\n", error.c_str());
		return 1;
	}
	if (phrases) {
		list_phrases(lang, stdout);
		return 0;
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (!parse_only)
		load_alarms();
	if (test_mode) {
		char line[1024];

		while (fgets(line, sizeof(line), stdin)) {
			line[strcspn(line, "\r\n")] = 0;
			handle(line);
		}
		return 0;
	}

	std::string fifo = run_dir + "/lidar-assistant.fifo", pending;
	bool oversized = false;	// skipping the rest of a line that was too long
	struct stat st;
	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);
	// read-write: the FIFO never reports end of file between writers. A
	// regular file of that name (written while the daemon was down) goes.
	struct pollfd fds[2] = { { -1, POLLIN, 0 }, { -1, POLLIN, 0 } };
	for (int attempt = 0; attempt < 2 && fds[0].fd < 0; attempt++) {
		if (mkfifo(fifo.c_str(), 0600) && errno != EEXIST)
			break;
		fds[0].fd = open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fds[0].fd >= 0 && (fstat(fds[0].fd, &st) || !S_ISFIFO(st.st_mode))) {
			close(fds[0].fd);
			fds[0].fd = -1;
			errno = EEXIST;
			if (unlink(fifo.c_str()))
				break;
		}
	}
	if (fds[0].fd < 0) {
		fprintf(stderr, "lidar-assistant: %s: %s\n", fifo.c_str(), strerror(errno));
		return 1;
	}
	fds[1].fd = open_buttons();
	tick();
	while (!quit) {
		char buf[1024];
		InputEvent event;
		ssize_t n;

		if (poll(fds, 2, timeout()) < 0 && errno != EINTR)
			break;
		while ((n = read(fds[0].fd, buf, sizeof(buf))) > 0) {
			pending.append(buf, (size_t)n);
			for (size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
				if (!oversized)
					handle(pending.substr(0, eol));
				oversized = false;
				pending.erase(0, eol + 1);
			}
			if (oversized || pending.size() > sizeof(buf)) {
				oversized = true;
				pending.clear();
			}
		}
		// any button pressed stops the ringing
		while (fds[1].fd >= 0 && (n = read(fds[1].fd, &event, sizeof(event))) == sizeof(event))
			if (event.type == 1 && event.value == 1)
				stop_ringing();
		if (fds[1].fd >= 0 && n < 0 && errno != EAGAIN && errno != EINTR) {
			close(fds[1].fd);	// the device is gone
			fds[1].fd = -1;
		}
		tick();
	}
	stop_ringing();
	unlink((run_dir + "/lidar-assistant.json").c_str());
	return 0;
}
