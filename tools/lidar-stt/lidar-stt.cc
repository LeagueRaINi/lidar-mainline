// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lidar-stt: speech recognition for the voice assistant. It keeps the
 * streaming recognizer of the installed language loaded and turns what is
 * said after the wake word into one line of text for lidar-assistant.
 *
 *   audio      16 kHz 16-bit mono on stdin, all the time: lidar-voice's
 *              output, which lidar-tee copies into /run/lidar-stt.pcm. The
 *              last 6 s are kept in a ring; nothing else is done with it
 *              while nobody speaks to the Echo
 *   wake       lidar-wake writes the time of each detection (seconds since
 *              boot, as lidar-wakeword prints it) to
 *              /run/lidar-wake.listening, which is looked at ten times a
 *              second. A new time opens a window: recognition starts at the
 *              audio of that moment less a pre-roll, out of the ring, so a
 *              command said right after "Hey Jarvis" keeps its start
 *   recognizer sherpa-onnx's streaming transducer ("Kroko" zipformer) from
 *              <models>/stt, greedy search, on 2 threads. The utterance ends
 *              at the recognizer's endpoint: 0.8 s of silence after speech,
 *              or 10 s of speech
 *   text       a leading "Hey Jarvis", which the pre-roll may catch, is
 *              dropped. What is left goes as one line to
 *              /run/lidar-assistant.fifo if lidar-assistant reads it (if not,
 *              it is lost); an empty result is dropped. If only the wake word
 *              was heard, recognition goes on until 5 s after the wake word
 *              without anything else said
 *   state      /run/lidar-stt.json, replaced whenever it changes:
 *                {"state": ..., "lang": ..., "partial": ..., "final": ...,
 *                 "seq": n}
 *              state is nomodel (none installed), loading (about 36 s on the
 *              Echo), failed (the model would not load), idle, listening
 *              (window open, nothing recognized yet) or decoding (partial
 *              is the text so far); final is the last line recognized and
 *              seq counts them
 *
 * <models> is lidar-models' symlink to the installed language (stt/ and
 * info, whose lang= goes into the state). While there is none, lidar-stt
 * looks again every 5 s; when the symlink points elsewhere (another
 * language installed), it starts itself again with the new model. A model
 * that fails to load is tried again only once the symlink has changed.
 *
 * This file is GPL-2.0-or-later, as lidar-wakeword.cc: the binary links
 * sherpa-onnx (Apache-2.0), which combines with GPL version 3, not with
 * version 2 alone.
 *
 * Usage: lidar-stt [-m models] [-l listening-file] [-o fifo|-] [-s state-file]
 *                  [-t threads] [-p seconds] [-w seconds] [-e seconds]
 *                  [-x seconds] [-W seconds] [-v]
 *   -m  default /var/lib/lidar-models/current
 *   -l  default /run/lidar-wake.listening
 *   -o  where the text goes, default /run/lidar-assistant.fifo; - for stdout
 *   -s  default /run/lidar-stt.json
 *   -t  recognizer threads, default 2
 *   -p  pre-roll before the detection, default 0.15 s (at most 3): more
 *       catches the end of "Jarvis" ("S."), none the start of a command run
 *       on without a pause
 *   -w  time to start speaking after the wake word, default 5 s
 *   -e  silence that ends an utterance, default 0.8 s
 *   -x  longest utterance, default 10 s
 *   -W  instead of watching the listening file, a wake word at this time of
 *       the input (seconds), which is read as fast as it comes, its time
 *       taken from the samples (for tests on recordings)
 *   -v  wake words, partial results and timings on stderr
 */
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sherpa-onnx/c-api/c-api.h"

namespace {

constexpr int kRate = 16000;
constexpr size_t kRing = 6 * kRate;	/* samples kept: pre-roll and lidar-wake's delay */
constexpr double kMaxPreroll = 3;
constexpr double kCheckInterval = 0.1;	/* listening file */
constexpr double kModelInterval = 5;	/* symlink, missing model */
constexpr size_t kMaxLine = 1000;	/* bytes; lidar-assistant takes 1023 */

struct Options {
	std::string models = "/var/lib/lidar-models/current";
	std::string listening = "/run/lidar-wake.listening";
	std::string out = "/run/lidar-assistant.fifo";
	std::string state = "/run/lidar-stt.json";
	int threads = 2;
	double preroll = 0.15, window = 5, silence = 0.8, longest = 10, test_wake = -1;
	bool verbose = false;
};

Options opt;
uint64_t pos;	/* samples read so far */

double uptime()
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the time of the newest sample: the input's own in test mode */
double now()
{
	return opt.test_wake >= 0 ? static_cast<double>(pos) / kRate : uptime();
}

bool read_text(const std::string &path, std::string &text)
{
	FILE *f = fopen(path.c_str(), "re");
	char buf[256];
	size_t n;

	if (!f)
		return false;
	text.clear();
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		text.append(buf, n);
	fclose(f);
	return true;
}

bool exists(const std::string &path)
{
	struct stat st;

	return stat(path.c_str(), &st) == 0;
}

/* ---- state file ---- */

struct State {
	const char *state = "loading";
	std::string lang, partial, final;
	unsigned seq = 0;
} state;

void json_string(std::string &out, const std::string &s)
{
	out += '"';
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += static_cast<char>(c);
		} else if (c < 0x20) {
			char esc[8];
			snprintf(esc, sizeof esc, "\\u%04x", c);
			out += esc;
		} else {
			out += static_cast<char>(c);
		}
	}
	out += '"';
}

void write_state()
{
	std::string j = "{\"state\": \"";

	j += state.state;
	j += "\", \"lang\": ";
	json_string(j, state.lang);
	j += ", \"partial\": ";
	json_string(j, state.partial);
	j += ", \"final\": ";
	json_string(j, state.final);
	j += ", \"seq\": " + std::to_string(state.seq) + "}\n";

	std::string tmp = opt.state + ".new";
	FILE *f = fopen(tmp.c_str(), "we");

	if (!f)
		return;
	bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
	if (fclose(f) || !ok || rename(tmp.c_str(), opt.state.c_str()))
		unlink(tmp.c_str());
}

void set_state(const char *s, const std::string &partial = "")
{
	if (state.state == s && state.partial == partial)
		return;
	state.state = s;
	state.partial = partial;
	write_state();
}

/*
 * The LED ring (lidar-ring): "wake think" once the text has gone to the
 * assistant, "wake off" when nothing was said. Opened read-write and
 * non-blocking, so a missing ring service never holds this up.
 */
void tell_ring(const char *cmd)
{
	int fd = open("/run/lidar-ring.fifo", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	struct stat st;

	if (fd < 0)
		return;
	if (!fstat(fd, &st) && S_ISFIFO(st.st_mode)) {
		std::string line = std::string(cmd) + "\n";
		ssize_t n = write(fd, line.data(), line.size());
		(void)n;
	}
	close(fd);
}

/* ---- text ---- */

std::string lower_word(const std::string &s, size_t from, size_t to)
{
	std::string w = s.substr(from, to - from);

	for (char &c : w)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return w;
}

bool word_char(unsigned char c)
{
	return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       c == '\'';
}

bool one_of(const std::string &w, const char *const *list)
{
	for (; *list; list++)
		if (w == *list)
			return true;
	return false;
}

/*
 * The text without a leading wake word, which the pre-roll may have caught
 * ("Jarvis, what time is it?"), and without the punctuation before what is
 * left. "Hey" goes only together with a "Jarvis" or alone.
 */
std::string strip_wake_word(const std::string &text)
{
	static const char *const hey[] = { "hey", "hay", "hei", "hi", "he", nullptr };
	static const char *const jarvis[] = { "jarvis", "jervis", "javis", "jarvi", "jarvys", "charvis",
					      "travis", "harvis", "garvis", nullptr };
	size_t at = 0;
	std::vector<std::string> words;
	std::vector<size_t> ends;

	/* the first two words and where they end */
	for (size_t i = 0; i < text.size() && words.size() < 2;) {
		while (i < text.size() && !word_char(text[i]))
			i++;
		size_t start = i;
		while (i < text.size() && word_char(text[i]))
			i++;
		if (i > start) {
			words.push_back(lower_word(text, start, i));
			ends.push_back(i);
		}
	}
	if (!words.empty() && one_of(words[0], jarvis))
		at = ends[0];
	else if (words.size() == 2 && one_of(words[0], hey) && one_of(words[1], jarvis))
		at = ends[1];
	else if (words.size() == 1 && one_of(words[0], hey))
		at = ends[0];
	while (at < text.size() && !word_char(text[at]))
		at++;
	return text.substr(at);
}

/* one line, no control characters, cut at a character boundary */
std::string one_line(const std::string &text)
{
	std::string s;

	for (unsigned char c : text)
		s += c < 0x20 ? ' ' : static_cast<char>(c);
	if (s.size() > kMaxLine) {
		size_t n = kMaxLine;
		while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80)
			n--;
		s.resize(n);
	}
	return s;
}

/* whether the line reached a reader */
bool send(const std::string &text)
{
	std::string line = one_line(text) + "\n";

	if (opt.out == "-") {
		fputs(line.c_str(), stdout);
		return fflush(stdout) == 0;
	}
	/* only a FIFO somebody reads: no reader gives ENXIO */
	int fd = open(opt.out.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	struct stat st;
	bool sent = false;

	if (fd < 0) {
		if (opt.verbose)
			fprintf(stderr, "lidar-stt: %s: %s, text dropped\n", opt.out.c_str(), strerror(errno));
		return false;
	}
	if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
		/* up to PIPE_BUF bytes are written whole or not at all */
		sent = write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
		if (!sent && opt.verbose)
			fprintf(stderr, "lidar-stt: %s: %s, text dropped\n", opt.out.c_str(), strerror(errno));
	} else if (opt.verbose) {
		fprintf(stderr, "lidar-stt: %s is no FIFO, text dropped\n", opt.out.c_str());
	}
	close(fd);
	return sent;
}

/* ---- the model ---- */

std::string model_target()
{
	char buf[PATH_MAX];

	return realpath(opt.models.c_str(), buf) ? buf : "";
}

const SherpaOnnxOnlineRecognizer *load()
{
	std::string stt = opt.models + "/stt/", info;
	std::string enc = stt + "encoder.onnx", dec = stt + "decoder.onnx", join = stt + "joiner.onnx",
		    tokens = stt + "tokens.txt";

	state.lang.clear();
	if (read_text(opt.models + "/info", info)) {
		size_t at = info.find("lang=");
		if (at != std::string::npos && (at == 0 || info[at - 1] == '\n'))
			state.lang = info.substr(at + 5, info.find('\n', at) - at - 5);
	}

	SherpaOnnxOnlineRecognizerConfig c;

	memset(&c, 0, sizeof c);
	c.feat_config.sample_rate = kRate;
	c.feat_config.feature_dim = 80;
	c.model_config.transducer.encoder = enc.c_str();
	c.model_config.transducer.decoder = dec.c_str();
	c.model_config.transducer.joiner = join.c_str();
	c.model_config.tokens = tokens.c_str();
	c.model_config.num_threads = opt.threads;
	c.model_config.provider = "cpu";
	c.decoding_method = "greedy_search";
	c.enable_endpoint = 1;
	/* rule 1: nothing but silence (or the wake word, see main) */
	c.rule1_min_trailing_silence = static_cast<float>(opt.window);
	c.rule2_min_trailing_silence = static_cast<float>(opt.silence);
	c.rule3_min_utterance_length = static_cast<float>(opt.longest);

	double t0 = uptime();
	const SherpaOnnxOnlineRecognizer *rec = SherpaOnnxCreateOnlineRecognizer(&c);

	if (rec && opt.verbose)
		fprintf(stderr, "lidar-stt: %s model loaded in %.1f s, %d threads\n", state.lang.c_str(),
			uptime() - t0, opt.threads);
	return rec;
}

/* ---- the wake word ---- */

struct Watch {
	struct timespec mtime = {};
	ino_t ino = 0;
	off_t size = -1;
	std::string handled;	/* the last time read */
} watch;

/*
 * The time of a new detection in the listening file, or -1. A file that is
 * being written (empty) is read again at the next look.
 */
double new_wake()
{
	struct stat st;
	std::string text;

	if (stat(opt.listening.c_str(), &st))
		return -1;
	if (st.st_ino == watch.ino && st.st_size == watch.size && st.st_mtim.tv_sec == watch.mtime.tv_sec &&
	    st.st_mtim.tv_nsec == watch.mtime.tv_nsec)
		return -1;
	if (!read_text(opt.listening, text))
		return -1;
	char *end;
	double t = strtod(text.c_str(), &end);
	if (end == text.c_str())
		return -1;
	watch.ino = st.st_ino;
	watch.size = st.st_size;
	watch.mtime = st.st_mtim;
	if (text == watch.handled)
		return -1;
	watch.handled = text;
	return t;
}

void usage()
{
	fprintf(stderr, "usage: lidar-stt [-m models] [-l listening-file] [-o fifo|-] [-s state-file]\n"
			"                 [-t threads] [-p seconds] [-w seconds] [-e seconds] [-x seconds]\n"
			"                 [-W seconds] [-v]\n");
	exit(2);
}

double seconds_arg(double max)
{
	char *end;
	double v = strtod(optarg, &end);

	if (*end || !(v >= 0 && v <= max))
		usage();
	return v;
}

}  // namespace

int main(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "m:l:o:s:t:p:w:e:x:W:v")) != -1) {
		switch (c) {
		case 'm': opt.models = optarg; break;
		case 'l': opt.listening = optarg; break;
		case 'o': opt.out = optarg; break;
		case 's': opt.state = optarg; break;
		case 't': opt.threads = static_cast<int>(seconds_arg(8)); break;
		case 'p': opt.preroll = seconds_arg(kMaxPreroll); break;
		case 'w': opt.window = seconds_arg(60); break;
		case 'e': opt.silence = seconds_arg(10); break;
		case 'x': opt.longest = seconds_arg(60); break;
		case 'W': opt.test_wake = seconds_arg(1e6); break;
		case 'v': opt.verbose = true; break;
		default: usage();
		}
	}
	if (optind != argc || opt.threads < 1)
		usage();

	/*
	 * Wait for a model: lidar-models may install one any time. One that
	 * fails to load is tried again only once the symlink points elsewhere
	 * (another install), not over and over at full CPU.
	 */
	std::string target;
	const SherpaOnnxOnlineRecognizer *rec = nullptr;

	for (;;) {
		target = model_target();
		if (!exists(opt.models + "/stt/encoder.onnx")) {
			set_state("nomodel");
			sleep(static_cast<unsigned>(kModelInterval));
			continue;
		}
		set_state("loading");
		rec = load();
		if (rec)
			break;
		fprintf(stderr, "lidar-stt: cannot load the model in %s/stt%s\n", opt.models.c_str(),
			opt.test_wake < 0 ? ", waiting for another one" : "");
		if (opt.test_wake >= 0)
			return 1;
		set_state("failed");
		while (model_target() == target)
			sleep(static_cast<unsigned>(kModelInterval));
	}
	/* a detection from before the start is not ours */
	if (opt.test_wake < 0)
		new_wake();
	set_state("idle");

	std::vector<int16_t> ring(kRing);
	std::vector<float> chunk(kRate / 10);
	char in[4096];
	size_t carry = 0;	/* a half sample from the last read */
	const SherpaOnnxOnlineStream *stream = nullptr;
	uint64_t start = 0, fed = 0;	/* samples: where this utterance starts, how far fed */
	double next_check = 0, next_model = uptime() + kModelInterval, wake = 0;
	bool test_woken = false, eof = false;

	for (;;) {
		ssize_t n = read(STDIN_FILENO, in + carry, sizeof in - carry);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			fprintf(stderr, "lidar-stt: stdin: %s\n", strerror(errno));
			return 1;
		}
		if (n == 0) {
			/* the end of a recording: what was said so far is final */
			if (!stream)
				break;
			eof = true;
			SherpaOnnxOnlineStreamInputFinished(stream);
		}
		size_t bytes = carry + static_cast<size_t>(n), m = bytes / 2;

		for (size_t i = 0; i < m; i++) {
			int16_t s;
			memcpy(&s, in + 2 * i, 2);
			ring[(pos + i) % kRing] = s;
		}
		pos += m;
		carry = bytes % 2;
		if (carry)
			in[0] = in[bytes - 1];

		double t = now();
		double w = -1;

		if (opt.test_wake >= 0) {
			if (!test_woken && t >= opt.test_wake) {
				w = opt.test_wake;
				test_woken = true;
			}
		} else if (!eof && t >= next_check) {
			next_check = t + kCheckInterval;
			w = new_wake();
		}
		/* a detection opens a window, also while one is open */
		double back = t - w + opt.preroll;
		if (w >= 0 && back <= static_cast<double>(kRing) / kRate) {
			uint64_t have = pos < kRing ? pos : kRing;
			uint64_t want = back > 0 ? static_cast<uint64_t>(back * kRate) : 0;

			if (stream)
				SherpaOnnxDestroyOnlineStream(stream);
			stream = SherpaOnnxCreateOnlineStream(rec);
			start = fed = pos - (want < have ? want : have);
			wake = w;
			if (opt.verbose)
				fprintf(stderr, "lidar-stt: wake word at %.2f, seen %.2f s later, from %.2f s back\n",
					w, t - w, static_cast<double>(pos - start) / kRate);
			set_state("listening");
		} else if (w >= 0 && opt.verbose) {
			fprintf(stderr, "lidar-stt: wake word at %.2f is too old (now %.2f)\n", w, t);
		}

		if (!stream) {
			/* idle: has lidar-models switched the language? */
			if (opt.test_wake < 0 && uptime() >= next_model) {
				next_model = uptime() + kModelInterval;
				if (model_target() != target) {
					fprintf(stderr, "lidar-stt: %s changed, loading again\n", opt.models.c_str());
					SherpaOnnxDestroyOnlineRecognizer(rec);
					execv("/proc/self/exe", argv);
					return 1;
				}
			}
			continue;
		}

		while (fed < pos) {
			size_t k = 0;
			for (; k < chunk.size() && fed < pos; k++, fed++)
				chunk[k] = ring[fed % kRing] / 32768.0f;
			SherpaOnnxOnlineStreamAcceptWaveform(stream, kRate, chunk.data(), static_cast<int32_t>(k));
		}
		while (SherpaOnnxIsOnlineStreamReady(rec, stream))
			SherpaOnnxDecodeOnlineStream(rec, stream);

		const SherpaOnnxOnlineRecognizerResult *r = SherpaOnnxGetOnlineStreamResult(rec, stream);
		std::string heard = r->text, text = strip_wake_word(heard);
		SherpaOnnxDestroyOnlineRecognizerResult(r);
		double spoken = static_cast<double>(fed - start) / kRate;

		if (text.empty()) {
			set_state("listening");
		} else if (text != state.partial) {
			if (opt.verbose)
				fprintf(stderr, "lidar-stt: %.2f partial: %s\n", spoken, text.c_str());
			set_state("decoding", text);
		}
		if (!eof && !SherpaOnnxOnlineStreamIsEndpoint(rec, stream))
			continue;
		if (!eof && text.empty() && spoken < opt.window) {
			/* only the wake word so far: go on listening */
			if (opt.verbose && !heard.empty())
				fprintf(stderr, "lidar-stt: %.2f only the wake word: %s\n", spoken, heard.c_str());
			SherpaOnnxOnlineStreamReset(rec, stream);
			continue;
		}
		if (opt.verbose)
			fprintf(stderr, "lidar-stt: %.2f %s%s (wake word +%.2f s)\n", spoken,
				text.empty() ? "nothing said" : "final: ", text.c_str(), now() - wake);
		SherpaOnnxDestroyOnlineStream(stream);
		stream = nullptr;
		bool sent = false;
		if (!text.empty()) {
			sent = send(text);
			state.final = text;
			state.seq++;
		}
		if (opt.test_wake < 0)
			tell_ring(sent ? "wake think" : "wake off");
		state.state = "idle";
		state.partial.clear();
		write_state();
		if (eof)
			break;
	}
	SherpaOnnxDestroyOnlineRecognizer(rec);
	return 0;
}
