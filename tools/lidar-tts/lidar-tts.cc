// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lidar-tts: the voice of the assistant. It keeps the Piper voice of the
 * installed language loaded and speaks what lidar-say queues, through the
 * speaker.
 *
 *   input      /run/lidar-tts.fifo, one line each (lidar-say writes them):
 *                say <language> <part>|<part>|...
 *                stop
 *              An answer comes in parts: fixed phrases, number words and
 *              names (lidar-assistant's LIDAR_SAY_PARTS), or one sentence
 *              of lidar-claude as one part; "." is a pause between
 *              sentences. Answers are spoken in order. "stop" (the wake
 *              word) drops them all and cuts off what is playing. An answer
 *              in a language other than the voice's is dropped
 *   voice      sherpa-onnx's VITS with the Piper voice of <models>/tts, the
 *              settings of <models>/info (noise_scale, noise_w,
 *              length_scale). The phonemes come from espeak-ng (voice
 *              espeak_voice of info), as Piper makes them: clause by clause
 *              with its punctuation, language switches dropped, decomposed
 *              (NFD), and the vowel clusters of tts/clusters.txt merged into
 *              their code points (see lidar-models). espeak-ng only
 *              phonemizes; it never speaks
 *   cache      each part's audio, trimmed of silence, by (voice, model,
 *              settings, text) in <models>/cache, so it goes with the
 *              language; the least recently used files go above the size
 *              limit. A part is synthesized only if it is not there, and
 *              the first part plays while the next are synthesized. After
 *              loading, the phrases and number words of the assistant
 *              (lidar-assistant -P) and Claude's fixed sentences
 *              (lidar-claude -P) that are not in the cache are prepared,
 *              one at a time at nice 19, only while no answer is queued or
 *              playing and nobody speaks to the Echo
 *              (lidar-wake.listening). Answers never wait for that but for
 *              the one phrase the model may be at, which then goes on at
 *              normal priority. About 15-20 minutes on the Echo, once per
 *              voice, unless lidar-models brought the cache of the release
 *              (made with -B on a PC, tools/mk-voicecache.sh)
 *   output     the ALSA device lidar_speaker, 48 kHz stereo (resampled
 *              here), at the system volume. As the other sounds: nothing
 *              while the speaker is muted (lidar-volume.state); 0.6 s of
 *              silence first if the codec output is closed, as the
 *              amplifier needs that long; the sound display keeps off it
 *              (lidar-viz.quiet)
 *   ring       "wake speak" to lidar-ring when it starts speaking (again
 *              every 20 s while it does), "wake off" when all is said;
 *              not after "stop" or a new wake word
 *              (lidar-wake.listening), which own the ring then. "busy
 *              prepare on" while phrases are prepared (again every 20 s),
 *              "busy prepare off" when that ends, pauses or lidar-tts
 *              goes
 *   state      /run/lidar-tts.json, replaced whenever it changes:
 *                {"state": ..., "lang": ..., "queue": n,
 *                 "prepare": {"done": n, "total": n}}
 *              state is nomodel (none installed, or it does not load),
 *              loading (about 11 s on the Echo), idle or speaking; queue
 *              counts the answers not yet spoken to the end; prepare is
 *              there while phrases are being prepared
 *   log        status lines, never text, to the kernel log
 *
 * <models> is lidar-models' symlink to the installed language. While there
 * is none, lidar-tts looks again every 5 s; when it points elsewhere,
 * lidar-tts starts itself again once it is idle (or at once if the voice
 * did not load). Answers that come while the voice loads wait for it.
 *
 * This file is GPL-2.0-or-later: the binary links sherpa-onnx (Apache-2.0)
 * and espeak-ng (GPL-3.0-or-later), which combine with GPL version 3, not
 * with version 2 alone.
 *
 * Usage: lidar-tts [-m models] [-r run-dir] [-D pcm | -w file.wav]
 *                  [-e espeak-data] [-a program] [-A program] [-t threads]
 *                  [-c megabytes] [-v]
 *        lidar-tts [-m models] [-e espeak-data] [-a program] [-A program]
 *                  [-t threads] [-c megabytes] -B cache-dir
 *        lidar-tts [-m models] [-e espeak-data] [-t threads] [-w file.wav]
 *                  -I text
 *   -m  default /var/lib/lidar-models/current
 *   -r  runtime files, default /run: lidar-tts.fifo, lidar-tts.json,
 *       lidar-ring.fifo, lidar-wake.listening, lidar-volume.state,
 *       lidar-viz.quiet
 *   -D  ALSA device, default lidar_speaker
 *   -w  instead of a device, append the sound to a WAV file (48 kHz
 *       stereo) in real time, for tests
 *   -e  espeak-ng's data directory, default /usr/share/espeak-ng-data
 *       (build.sh builds it)
 *   -a  what lists the assistant's phrases: <program> -l <language> -P, one
 *       per line (default lidar-assistant; - for none)
 *   -A  what lists Claude's fixed sentences: <program> -P <language>
 *       (default lidar-claude; - for none)
 *   -t  threads of the voice, default 3
 *   -c  cache size, default 20 MB
 *   -v  timings on stderr
 *   -B  render the phrases of -a and -A that are not there yet into this
 *       cache directory, as the daemon would into <models>/cache, and exit:
 *       a cache made on a PC (tools/mk-voicecache.sh) for the same voice
 *       and model is the Echo's. Fails if a list program fails
 *   -I  print the phonemes and phoneme ids of the text, synthesize it twice
 *       with timings (into -w's file, if given), and exit; "-I -": only the
 *       phonemes and ids of each line of stdin
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <espeak-ng/speak_lib.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nfd.h"
#include "sherpa-onnx/c-api/c-api.h"

namespace {

constexpr int kOutRate = 48000;
constexpr double kModelInterval = 5;	/* symlink, missing model */
constexpr double kPartGap = 0.08;	/* between the parts of an answer */
constexpr double kSentenceGap = 0.35;	/* a "." part */
constexpr double kAnswerGap = 0.3;	/* between answers */
constexpr double kCodecWake = 0.6;	/* silence first if the codec output is closed */
constexpr double kVizMargin = 0.3;	/* speaker tuning and buffers */
constexpr double kSpeakRepeat = 20;	/* lidar-ring holds "speak" for 60 s */
constexpr float kPeak = 0.7f;		/* parts are normalized to -3 dBFS */
constexpr float kSilence = 0.01f;	/* trimmed at the ends: -40 dB below the peak */
constexpr int kPresynthNice = 19;
constexpr size_t kChunk = 1024;		/* frames per write */
constexpr const char *kCodecStatus = "/proc/asound/amazonlidar/pcm0p/sub0/status";
/* what the cached audio depends on besides voice, model and text; raise it
 * when the phonemes or the processing change */
constexpr int kCacheVersion = 2;

struct Options {
	std::string models = "/var/lib/lidar-models/current";
	std::string run = "/run";
	std::string device = "lidar_speaker";
	std::string wav;
	std::string espeak_data;
	std::string assistant = "lidar-assistant", claude = "lidar-claude";
	std::string say_text, batch;
	int threads = 3;
	size_t cache_bytes = 20u << 20;
	bool verbose = false, test = false;
} opt;

double uptime()
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A status line for the kernel log, or stderr where there is none */
__attribute__((format(printf, 1, 2))) void status(const char *fmt, ...)
{
	static int kmsg = opt.batch.empty() ? open("/dev/kmsg", O_WRONLY | O_CLOEXEC) : -1;
	char line[300];
	va_list ap;
	int n = snprintf(line, sizeof line, "lidar-tts: ");

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof line - static_cast<size_t>(n) - 1, fmt, ap);
	va_end(ap);
	n = static_cast<int>(strlen(line));
	line[n++] = '\n';
	if (write(kmsg >= 0 ? kmsg : 2, line, static_cast<size_t>(n)) < 0) {
		/* nowhere else to report it */
	}
	if (opt.verbose && kmsg >= 0)
		fputs(line, stderr);
}

__attribute__((format(printf, 1, 2))) void verbose(const char *fmt, ...)
{
	va_list ap;

	if (!opt.verbose)
		return;
	fprintf(stderr, "lidar-tts: %.3f ", uptime());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

bool read_text(const std::string &path, std::string &text)
{
	FILE *f = fopen(path.c_str(), "re");
	char buf[4096];
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

/* key=value of lidar-models' info file */
std::string info_value(const std::string &info, const std::string &key)
{
	for (size_t at = 0; at < info.size();) {
		size_t end = info.find('\n', at);
		if (end == std::string::npos)
			end = info.size();
		if (!info.compare(at, key.size() + 1, key + "="))
			return info.substr(at + key.size() + 1, end - at - key.size() - 1);
		at = end + 1;
	}
	return "";
}

std::string realpath_of(const std::string &path)
{
	char buf[PATH_MAX];

	return realpath(path.c_str(), buf) ? buf : "";
}

/* ---- UTF-8 ---- */

std::u32string utf8_decode(const std::string &s)
{
	std::u32string out;

	for (size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		char32_t cp = n == 3 ? c & 7 : n == 2 ? c & 15 : n == 1 ? c & 31 : c;
		bool ok = n >= 0 && i + static_cast<size_t>(n) < s.size();

		for (int k = 1; ok && k <= n; k++) {
			unsigned char d = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
			ok = (d & 0xc0) == 0x80;
			cp = cp << 6 | (d & 0x3f);
		}
		if (ok)
			out += cp;
		i += ok ? static_cast<size_t>(n) + 1 : 1;
	}
	return out;
}

void utf8_append(std::string &s, char32_t c)
{
	if (c < 0x80) {
		s += static_cast<char>(c);
	} else if (c < 0x800) {
		s += static_cast<char>(0xc0 | c >> 6);
		s += static_cast<char>(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		s += static_cast<char>(0xe0 | c >> 12);
		s += static_cast<char>(0x80 | (c >> 6 & 0x3f));
		s += static_cast<char>(0x80 | (c & 0x3f));
	} else {
		s += static_cast<char>(0xf0 | c >> 18);
		s += static_cast<char>(0x80 | (c >> 12 & 0x3f));
		s += static_cast<char>(0x80 | (c >> 6 & 0x3f));
		s += static_cast<char>(0x80 | (c & 0x3f));
	}
}

/*
 * c decomposed, as Piper does (Unicode NFD: "ç" is "c" and U+0327), when the
 * voice has every part of it; else as it is, if the voice has that: voices
 * of Piper before 1.0 (eva_k) were trained on espeak-ng's phonemes as they
 * come and have "ç" but no U+0327.
 */
void nfd_append(std::vector<char32_t> &out, char32_t c, const std::unordered_map<char32_t, int> &tokens)
{
	const auto *end = kNfd + sizeof kNfd / sizeof kNfd[0];
	const auto *e = std::lower_bound(kNfd, end, c,
					 [](const char32_t(&row)[4], char32_t v) { return row[0] < v; });

	if (e == end || (*e)[0] != c) {
		out.push_back(c);
		return;
	}
	bool parts = true;
	for (int k = 1; k < 4 && (*e)[k]; k++)
		parts &= tokens.count((*e)[k]) > 0;
	if (!parts && tokens.count(c)) {
		out.push_back(c);
		return;
	}
	for (int k = 1; k < 4 && (*e)[k]; k++)
		out.push_back((*e)[k]);
}

/* ---- the voice ---- */

struct Audio {
	int rate = 0;
	std::vector<int16_t> samples;	/* mono */
};

struct Cluster {
	std::vector<char32_t> phonemes;
	char32_t code;
};

struct Voice {
	const SherpaOnnxOfflineTts *tts = nullptr;
	int rate = 0;
	std::string lang, name, espeak, dir;	/* dir: where the symlink pointed at load */
	long long model_bytes = 0;	/* model.onnx: with the name, which model it is */
	float noise = 0.667f, noise_w = 0.8f, length = 1;
	std::vector<Cluster> clusters;
	std::unordered_map<char32_t, int> tokens;	/* tokens.txt: phoneme -> id */
} voice;

float info_float(const std::string &info, const char *key, float fallback)
{
	std::string v = info_value(info, key);
	char *end;
	float f = strtof(v.c_str(), &end);

	return v.empty() || *end || !(f > 0) ? fallback : f;
}

/* info, clusters.txt, tokens.txt: what is known before the model loads */
bool read_voice()
{
	std::string info, text;

	if (!read_text(opt.models + "/info", info))
		return false;
	voice.dir = realpath_of(opt.models);
	voice.lang = info_value(info, "lang");
	voice.name = info_value(info, "voice");
	voice.espeak = info_value(info, "espeak_voice");
	voice.noise = info_float(info, "noise_scale", voice.noise);
	voice.noise_w = info_float(info, "noise_w", voice.noise_w);
	voice.length = info_float(info, "length_scale", voice.length);
	struct stat st;
	voice.model_bytes = stat((opt.models + "/tts/model.onnx").c_str(), &st) ? 0 : st.st_size;
	if (voice.espeak.empty())
		voice.espeak = voice.lang;
	voice.tokens.clear();
	if (read_text(opt.models + "/tts/tokens.txt", text)) {
		/* "<phoneme> <id>"; the phoneme may be a space */
		for (size_t at = 0; at < text.size();) {
			size_t end = text.find('\n', at);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			size_t sp = line.rfind(' ');
			std::u32string sym = utf8_decode(line.substr(0, sp == std::string::npos ? 0 : sp));
			if (sym.size() == 1)
				voice.tokens[sym[0]] = atoi(line.c_str() + sp + 1);
			at = end + 1;
		}
	}
	voice.clusters.clear();
	if (read_text(opt.models + "/tts/clusters.txt", text)) {
		for (size_t at = 0; at < text.size();) {
			size_t end = text.find('\n', at), sp;
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			at = end + 1;
			if ((sp = line.rfind(' ')) == std::string::npos || !sp)
				continue;
			std::u32string ph = utf8_decode(line.substr(0, sp)), code = utf8_decode(line.substr(sp + 1));
			if (ph.size() < 2 || code.size() != 1)
				continue;
			voice.clusters.push_back({ std::vector<char32_t>(ph.begin(), ph.end()), code[0] });
		}
		/* the longest match first, as Piper merges */
		std::stable_sort(voice.clusters.begin(), voice.clusters.end(),
				 [](const Cluster &a, const Cluster &b) { return a.phonemes.size() > b.phonemes.size(); });
	}
	return !voice.lang.empty();
}

/* clause terminators, as espeak-ng reports them (see Piper's espeakbridge.c) */
constexpr int kIntonationComma = 0x1000, kIntonationQuestion = 0x2000, kIntonationExclamation = 0x3000;
constexpr int kTypeClause = 0x40000, kTypeSentence = 0x80000;
constexpr int kPeriod = 40 | kTypeSentence;
constexpr int kComma = 20 | kIntonationComma | kTypeClause;
constexpr int kQuestion = 40 | kIntonationQuestion | kTypeSentence;
constexpr int kExclamation = 45 | kIntonationExclamation | kTypeSentence;
constexpr int kColon = 30 | kTypeClause;
constexpr int kSemicolon = 30 | kIntonationComma | kTypeClause;

void merge_clusters(std::vector<char32_t> &ph)
{
	if (voice.clusters.empty())
		return;
	std::vector<char32_t> out;

	for (size_t i = 0; i < ph.size();) {
		const Cluster *match = nullptr;
		for (const Cluster &c : voice.clusters) {
			if (c.phonemes.size() <= ph.size() - i &&
			    std::equal(c.phonemes.begin(), c.phonemes.end(), ph.begin() + static_cast<long>(i))) {
				match = &c;
				break;
			}
		}
		out.push_back(match ? match->code : ph[i]);
		i += match ? match->phonemes.size() : 1;
	}
	ph.swap(out);
}

/*
 * Text to phonemes, one list per sentence, as Piper's phonemize_espeak.py
 * makes them: espeak-ng's IPA clause by clause, language switches "(en)"
 * dropped, the clause's punctuation kept (a space after , : ;), every code
 * point decomposed, the vowel clusters merged per sentence.
 */
std::vector<std::vector<char32_t>> phonemize(const std::string &text)
{
	std::vector<std::vector<char32_t>> sentences;
	std::vector<char32_t> cur;
	const void *p = text.c_str();

	while (p) {
		int term = 0;
		const char *ph = espeak_TextToPhonemesWithTerminator(&p, espeakCHARS_AUTO, espeakPHONEMES_IPA, &term);
		std::string s;

		for (const char *c = ph ? ph : ""; *c; c++) {
			const char *close = *c == '(' ? strchr(c + 1, ')') : nullptr;
			if (close && close > c + 1)
				c = close;
			else
				s += *c;
		}
		term &= 0x000fffff;
		switch (term) {
		case kPeriod: s += "."; break;
		case kQuestion: s += "?"; break;
		case kExclamation: s += "!"; break;
		case kComma: s += ", "; break;
		case kColon: s += ": "; break;
		case kSemicolon: s += "; "; break;
		default: break;
		}
		for (char32_t c : utf8_decode(s))
			nfd_append(cur, c, voice.tokens);
		if ((term & kTypeSentence) == kTypeSentence) {
			merge_clusters(cur);
			if (!cur.empty())
				sentences.push_back(std::move(cur));
			cur.clear();
		}
	}
	merge_clusters(cur);
	if (!cur.empty())
		sentences.push_back(std::move(cur));
	return sentences;
}

bool load_voice()
{
	std::string tts = opt.models + "/tts/";
	std::string model = tts + "model.onnx", tokens = tts + "tokens.txt";

	if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, opt.espeak_data.empty() ? nullptr : opt.espeak_data.c_str(),
			      0) < 0) {
		status("espeak-ng: cannot load its data%s%s", opt.espeak_data.empty() ? "" : " from ",
		       opt.espeak_data.c_str());
		return false;
	}
	if (espeak_SetVoiceByName(voice.espeak.c_str()) != EE_OK) {
		status("espeak-ng has no voice %s", voice.espeak.c_str());
		return false;
	}

	SherpaOnnxOfflineTtsConfig c;

	memset(&c, 0, sizeof c);
	c.model.vits.model = model.c_str();
	c.model.vits.tokens = tokens.c_str();
	c.model.vits.noise_scale = voice.noise;
	c.model.vits.noise_scale_w = voice.noise_w;
	c.model.vits.length_scale = voice.length;
	c.model.num_threads = opt.threads;
	c.model.provider = "cpu";
	/* one sentence per run of the model, as Piper: the first can play sooner */
	c.max_num_sentences = 1;
	c.silence_scale = 1;
	voice.tts = SherpaOnnxCreateOfflineTts(&c);
	if (!voice.tts)
		return false;
	voice.rate = SherpaOnnxOfflineTtsSampleRate(voice.tts);
	return voice.rate > 0;
}

struct Abort {
	const std::atomic<unsigned> *gen;
	unsigned want;
	bool aborted;
};

int32_t keep_going(const float *, int32_t, float, void *arg)
{
	Abort *a = static_cast<Abort *>(arg);

	if (a->gen && a->gen->load() != a->want)
		a->aborted = true;
	return !a->aborted;
}

/*
 * The audio of one part: trimmed of the silence at its ends, normalized,
 * faded in and out. false if it failed, or was given up because gen moved
 * from want (a "stop"); an empty result for a text without phonemes.
 */
bool synthesize(const std::string &text, Audio &out, const std::atomic<unsigned> *gen, unsigned want,
		double *seconds = nullptr)
{
	std::vector<std::vector<char32_t>> sentences = phonemize(text);
	std::vector<int32_t> flat, lens;

	out.rate = voice.rate;
	out.samples.clear();
	for (const auto &s : sentences) {
		flat.insert(flat.end(), s.begin(), s.end());
		lens.push_back(static_cast<int32_t>(s.size()));
	}
	if (flat.empty())
		return true;

	SherpaOnnxGenerationConfig g;
	Abort abort = { gen, want, false };
	double t0 = uptime();

	memset(&g, 0, sizeof g);
	g.silence_scale = 1;
	g.speed = 1;
	g.phoneme_codepoints = flat.data();
	g.phoneme_codepoints_lens = lens.data();
	g.phoneme_codepoints_num_sentences = static_cast<int32_t>(lens.size());

	const SherpaOnnxGeneratedAudio *a = SherpaOnnxOfflineTtsGenerateWithConfig(voice.tts, "", &g, keep_going, &abort);

	if (seconds)
		*seconds = uptime() - t0;
	if (!a || abort.aborted) {
		SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
		return false;
	}
	float peak = 0;
	for (int32_t i = 0; i < a->n; i++)
		peak = std::max(peak, std::fabs(a->samples[i]));
	if (peak > 1e-4f) {
		int32_t first = 0, last = a->n - 1, rate = a->sample_rate;
		while (first < last && std::fabs(a->samples[first]) < kSilence * peak)
			first++;
		while (last > first && std::fabs(a->samples[last]) < kSilence * peak)
			last--;
		int32_t from = std::max(0, first - rate / 50), to = std::min(a->n, last + 1 + rate / 20);
		int32_t fade_in = std::min(first - from + rate / 200, to - from), fade_out = rate / 100;
		float gain = kPeak / peak;

		out.rate = rate;
		out.samples.resize(static_cast<size_t>(to - from));
		for (int32_t i = from; i < to; i++) {
			float v = a->samples[i] * gain;
			if (i - from < fade_in)
				v *= static_cast<float>(i - from) / static_cast<float>(fade_in);
			if (to - i <= fade_out)
				v *= static_cast<float>(to - i - 1) / static_cast<float>(fade_out);
			out.samples[static_cast<size_t>(i - from)] =
				static_cast<int16_t>(std::lrint(std::max(-32767.f, std::min(32767.f, v * 32767.f))));
		}
	}
	SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
	return true;
}

/* ---- the cache ---- */

class Cache {
public:
	/*
	 * The key of a file: the version of the processing, the voice, the size
	 * of its model.onnx (which lidar-models pins), the settings and the
	 * text. A cache of another model is never read; its files go as the
	 * least recently used.
	 */
	bool open(const std::string &dir, size_t cap)
	{
		std::lock_guard<std::mutex> lk(mu_);
		char k[200];

		dir_ = dir;
		cap_ = cap;
		snprintf(k, sizeof k, "%d %s %lld %g %g %g\n", kCacheVersion, voice.name.c_str(), voice.model_bytes,
			 voice.noise, voice.noise_w, voice.length);
		prefix_ = k;
		ok_ = (mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) && access(dir.c_str(), W_OK) == 0;
		if (!ok_) {
			status("%s: not writable, nothing is cached", dir.c_str());
			return false;
		}
		total_ = scan(nullptr);
		if (total_ > cap_) {	/* a smaller limit than before */
			unsigned gone = 0;
			total_ = scan(&gone);
		}
		return true;
	}

	/* the audio of text, or nullptr; a hit counts as a use */
	std::shared_ptr<const Audio> get(const std::string &text)
	{
		std::string key, data;
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (!ok_)
				return nullptr;
			key = prefix_ + text;
			std::string path = path_of(key);
			if (!read_text(path, data))
				return nullptr;
			utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
		}
		Header h;
		if (data.size() < sizeof h)
			return nullptr;
		memcpy(&h, data.data(), sizeof h);
		if (memcmp(h.magic, "LTTS", 4) || h.key != key.size() ||
		    data.size() != sizeof h + h.key + 2 * static_cast<size_t>(h.samples) ||
		    data.compare(sizeof h, h.key, key))
			return nullptr;
		auto a = std::make_shared<Audio>();
		a->rate = static_cast<int>(h.rate);
		a->samples.resize(h.samples);
		memcpy(a->samples.data(), data.data() + sizeof h + h.key, 2 * static_cast<size_t>(h.samples));
		return a;
	}

	/* whether text is there; counts as a use */
	bool touch(const std::string &text)
	{
		std::lock_guard<std::mutex> lk(mu_);

		return ok_ && utimensat(AT_FDCWD, path_of(prefix_ + text).c_str(), nullptr, 0) == 0;
	}

	void put(const std::string &text, const Audio &a)
	{
		std::lock_guard<std::mutex> lk(mu_);
		std::string key = prefix_ + text, path = path_of(key), tmp = path + ".tmp";
		Header h = { { 'L', 'T', 'T', 'S' }, static_cast<uint32_t>(a.rate), static_cast<uint32_t>(key.size()),
			     static_cast<uint32_t>(a.samples.size()) };
		struct stat st;

		if (!ok_)
			return;
		FILE *f = fopen(tmp.c_str(), "we");
		if (!f)
			return;
		bool good = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(key.data(), 1, key.size(), f) == key.size() &&
			    fwrite(a.samples.data(), 2, a.samples.size(), f) == a.samples.size();
		if (fclose(f) || !good) {
			unlink(tmp.c_str());
			return;
		}
		size_t size = sizeof h + key.size() + 2 * a.samples.size();
		if (stat(path.c_str(), &st) == 0)
			total_ -= std::min(total_, static_cast<size_t>(st.st_size));
		if (rename(tmp.c_str(), path.c_str())) {
			unlink(tmp.c_str());
			return;
		}
		total_ += size;
		if (total_ > cap_) {
			unsigned gone = 0;
			total_ = scan(&gone);
			verbose("cache: %u files removed, %zu bytes left", gone, total_);
		}
	}

	size_t bytes()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return total_;
	}

private:
	struct Header {
		char magic[4];
		uint32_t rate, key, samples;
	};

	std::mutex mu_;
	std::string dir_, prefix_;
	size_t cap_ = 0, total_ = 0;
	bool ok_ = false;

	std::string path_of(const std::string &key) const
	{
		uint64_t h = 0xcbf29ce484222325ull;	/* FNV-1a */
		char name[24];

		for (unsigned char c : key)
			h = (h ^ c) * 0x100000001b3ull;
		snprintf(name, sizeof name, "/%016llx", static_cast<unsigned long long>(h));
		return dir_ + name;
	}

	/* bytes in the cache; with gone, the oldest files go until it is 90 %
	 * of the limit; leftovers of an interrupted write always go */
	size_t scan(unsigned *gone)
	{
		struct File {
			struct timespec mtime;
			size_t size;
			std::string path;
		};
		std::vector<File> files;
		size_t total = 0;
		DIR *d = opendir(dir_.c_str());
		struct dirent *e;

		if (!d)
			return 0;
		while ((e = readdir(d))) {
			std::string name = e->d_name, path = dir_ + "/" + name;
			struct stat st;

			if (name[0] == '.' || stat(path.c_str(), &st) || !S_ISREG(st.st_mode))
				continue;
			if (name.size() != 16) {
				unlink(path.c_str());
				continue;
			}
			files.push_back({ st.st_mtim, static_cast<size_t>(st.st_size), path });
			total += static_cast<size_t>(st.st_size);
		}
		closedir(d);
		if (!gone)
			return total;
		std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
			return a.mtime.tv_sec != b.mtime.tv_sec ? a.mtime.tv_sec < b.mtime.tv_sec
								: a.mtime.tv_nsec < b.mtime.tv_nsec;
		});
		for (const File &f : files) {
			if (total <= cap_ / 10 * 9)
				break;
			if (unlink(f.path.c_str()) == 0) {
				total -= f.size;
				++*gone;
			}
		}
		return total;
	}
} cache;

/* ---- resampling to 48 kHz ---- */

/*
 * Polyphase windowed-sinc (Kaiser, beta 8, 32 taps per phase) from the
 * voice's rate to 48 kHz, mono to stereo.
 */
class Resampler {
public:
	void init(int rate)
	{
		int g = gcd(rate, kOutRate);

		up_ = kOutRate / g;
		down_ = rate / g;
		/* the cutoff, in cycles per input sample: below both Nyquists */
		double fc = 0.5 * std::min(1.0, static_cast<double>(up_) / down_) * 0.92;

		h_.assign(static_cast<size_t>(up_) * kTaps, 0);
		for (int p = 0; p < up_; p++) {
			double sum = 0;
			for (int j = 0; j < kTaps; j++) {
				double d = (j - kTaps / 2 + 1) - static_cast<double>(p) / up_;
				double x = d / (kTaps / 2), w = std::fabs(x) < 1 ? bessel_i0(8 * std::sqrt(1 - x * x)) / bessel_i0(8) : 0;
				double v = 2 * fc * (d == 0 ? 1 : std::sin(M_PI * 2 * fc * d) / (M_PI * 2 * fc * d)) * w;
				h_[static_cast<size_t>(p * kTaps + j)] = static_cast<float>(v);
				sum += v;
			}
			for (int j = 0; j < kTaps; j++)
				h_[static_cast<size_t>(p * kTaps + j)] /= static_cast<float>(sum);
		}
	}

	/* in: mono at the voice's rate; out: interleaved stereo at 48 kHz */
	void run(const std::vector<int16_t> &in, std::vector<int16_t> &out) const
	{
		size_t n = (in.size() * static_cast<size_t>(up_) + static_cast<size_t>(down_) - 1) /
			   static_cast<size_t>(down_);

		out.resize(2 * n);
		for (size_t k = 0; k < n; k++) {
			uint64_t pos = static_cast<uint64_t>(k) * static_cast<uint64_t>(down_);
			long i = static_cast<long>(pos / static_cast<uint64_t>(up_));
			const float *h = &h_[static_cast<size_t>(pos % static_cast<uint64_t>(up_)) * kTaps];
			float acc = 0;

			for (int j = 0; j < kTaps; j++) {
				long at = i - kTaps / 2 + 1 + j;
				if (at >= 0 && at < static_cast<long>(in.size()))
					acc += h[j] * in[static_cast<size_t>(at)];
			}
			int16_t v = static_cast<int16_t>(std::lrint(std::max(-32768.f, std::min(32767.f, acc))));
			out[2 * k] = out[2 * k + 1] = v;
		}
	}

private:
	static constexpr int kTaps = 32;
	int up_ = 1, down_ = 1;
	std::vector<float> h_;

	static int gcd(int a, int b) { return b ? gcd(b, a % b) : a; }

	static double bessel_i0(double x)
	{
		double sum = 1, term = 1;

		for (int k = 1; k < 30; k++) {
			term *= (x / (2 * k)) * (x / (2 * k));
			sum += term;
		}
		return sum;
	}
} resampler;

/* ---- the output ---- */

void wav_header(int fd, uint32_t data_bytes)
{
	unsigned char h[44];
	auto le = [&](int at, uint32_t v, int n) {
		for (int i = 0; i < n; i++)
			h[at + i] = static_cast<unsigned char>(v >> (8 * i));
	};

	memcpy(h, "RIFF\0\0\0\0WAVEfmt ", 16);
	le(4, 36 + data_bytes, 4);
	le(16, 16, 4);
	le(20, 1, 2);
	le(22, 2, 2);
	le(24, kOutRate, 4);
	le(28, kOutRate * 4, 4);
	le(32, 4, 2);
	le(34, 16, 2);
	memcpy(h + 36, "data", 4);
	le(40, data_bytes, 4);
	if (pwrite(fd, h, sizeof h, 0) != static_cast<ssize_t>(sizeof h)) {
		/* the test file stays as it is */
	}
}

/* lidar_speaker, or a WAV file written in real time (-w) */
class Output {
public:
	bool is_open() const { return pcm_ || fd_ >= 0; }

	bool open()
	{
		if (!opt.wav.empty()) {
			struct stat st;

			fd_ = ::open(opt.wav.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
			if (fd_ < 0 || fstat(fd_, &st)) {
				status("%s: %s", opt.wav.c_str(), strerror(errno));
				close(false);
				return false;
			}
			if (st.st_size < 44)
				wav_header(fd_, 0);
			lseek(fd_, 0, SEEK_END);
			t0_ = uptime();
			frames_ = 0;
			return true;
		}
		int err = snd_pcm_open(&pcm_, opt.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
		if (err >= 0)
			err = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
						 kOutRate, 1, 100000);
		if (err < 0) {
			status("%s: %s", opt.device.c_str(), snd_strerror(err));
			close(false);
			return false;
		}
		return true;
	}

	/* false if stopped (gen moved from want) or failed */
	bool write(const int16_t *lr, size_t frames, const std::atomic<unsigned> &gen, unsigned want)
	{
		while (frames) {
			size_t n = std::min(frames, kChunk);

			if (gen.load() != want)
				return false;
			if (fd_ >= 0) {
				if (::write(fd_, lr, 4 * n) != static_cast<ssize_t>(4 * n))
					return false;
				/* as a sound card with 0.1 s of buffer */
				frames_ += n;
				wait_until(t0_ + static_cast<double>(frames_) / kOutRate - 0.1);
			} else {
				snd_pcm_sframes_t w = snd_pcm_writei(pcm_, lr, n);
				if (w == -EAGAIN)
					continue;
				if (w < 0) {
					int err = snd_pcm_recover(pcm_, static_cast<int>(w), 1);
					if (err < 0) {
						status("%s: %s", opt.device.c_str(), snd_strerror(err));
						return false;
					}
					continue;
				}
				n = static_cast<size_t>(w);
			}
			lr += 2 * n;
			frames -= n;
		}
		return true;
	}

	void silence(double seconds, const std::atomic<unsigned> &gen, unsigned want)
	{
		std::vector<int16_t> zero(2 * kChunk);

		for (size_t n = static_cast<size_t>(seconds * kOutRate); n && gen.load() == want;) {
			size_t k = std::min(n, kChunk);
			if (!write(zero.data(), k, gen, want))
				break;
			n -= k;
		}
	}

	/* drain: let what was written play out; else cut it off */
	void close(bool drain)
	{
		if (pcm_) {
			if (drain)
				snd_pcm_drain(pcm_);
			else
				snd_pcm_drop(pcm_);
			snd_pcm_close(pcm_);
			pcm_ = nullptr;
		}
		if (fd_ >= 0) {
			struct stat st;
			if (drain)
				wait_until(t0_ + static_cast<double>(frames_) / kOutRate);
			if (fstat(fd_, &st) == 0 && st.st_size >= 44)
				wav_header(fd_, static_cast<uint32_t>(st.st_size - 44));
			::close(fd_);
			fd_ = -1;
		}
	}

private:
	snd_pcm_t *pcm_ = nullptr;
	int fd_ = -1;
	double t0_ = 0;
	uint64_t frames_ = 0;

	static void wait_until(double t)
	{
		double d = t - uptime();
		if (d > 0) {
			struct timespec ts = { static_cast<time_t>(d), static_cast<long>((d - std::floor(d)) * 1e9) };
			nanosleep(&ts, nullptr);
		}
	}
} out;

/* ---- the queue ---- */

struct Part {
	enum State { WAITING, BUSY, READY, FAILED };
	std::string text;
	State state = WAITING;
	bool pause = false;	/* "." */
	std::shared_ptr<const Audio> audio;
};

struct Answer {
	std::vector<std::shared_ptr<Part>> parts;
	double received;
	unsigned id;
	unsigned texts, cached;	/* parts with text; of those, in the cache */
};

enum class Model { NONE, LOADING, READY };

std::mutex mu;
std::condition_variable cv;
std::deque<std::shared_ptr<Answer>> queue;
std::atomic<unsigned> stop_gen{ 0 };
Model model = Model::NONE;
bool speaking = false;
std::vector<std::string> presynth;	/* the phrases not in the cache yet */
size_t presynth_next = 0;
std::atomic<pid_t> player_tid{ 0 };
volatile sig_atomic_t quit = 0;
char **args;	/* main's argv, to start again with */

/* with mu held */
void write_state()
{
	static std::string last;
	const char *s = model == Model::NONE ? "nomodel" : model == Model::LOADING ? "loading"
			: speaking ? "speaking" : "idle";
	std::string j = std::string("{\"state\": \"") + s + "\", \"lang\": \"" + voice.lang +
			"\", \"queue\": " + std::to_string(queue.size());
	if (presynth_next < presynth.size())
		j += ", \"prepare\": {\"done\": " + std::to_string(presynth_next) +
		     ", \"total\": " + std::to_string(presynth.size()) + "}";
	j += "}\n";

	if (j == last)
		return;
	last = j;
	std::string path = opt.run + "/lidar-tts.json", tmp = path + ".new";
	FILE *f = fopen(tmp.c_str(), "we");
	if (!f)
		return;
	bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
	if (fclose(f) || !ok || rename(tmp.c_str(), path.c_str()))
		unlink(tmp.c_str());
}

/* the first part of the queue that waits for synthesis, with mu held */
std::shared_ptr<Part> next_waiting()
{
	for (const auto &a : queue)
		for (const auto &p : a->parts)
			if (p->state == Part::WAITING)
				return p;
	return nullptr;
}

/*
 * Every thread but the player's at this nice value: the synthesis runs on
 * onnxruntime's threads too. With mu held: the synthesis thread and an
 * answer that comes in both set it.
 */
void renice(int nice)
{
	static int current = 0;
	DIR *d;
	struct dirent *e;

	if (nice == current || !(d = opendir("/proc/self/task")))
		return;
	current = nice;
	while ((e = readdir(d))) {
		int tid = atoi(e->d_name);
		if (tid > 0 && tid != player_tid.load())
			setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice);
	}
	closedir(d);
}

/* the lines <argv> prints; false if it fails, quietly if it is not there */
bool run_list(const std::vector<std::string> &argv, std::vector<std::string> &list, bool batch)
{
	std::vector<char *> av;
	int fd[2];

	for (const std::string &a : argv)
		av.push_back(const_cast<char *>(a.c_str()));
	av.push_back(nullptr);
	if (pipe2(fd, O_CLOEXEC))
		return false;
	pid_t pid = fork();
	if (!pid) {
		dup2(fd[1], 1);
		int null = ::open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 2);
		}
		execvp(av[0], av.data());
		_exit(127);
	}
	close(fd[1]);
	std::string text;
	char buf[4096];
	ssize_t n;
	while (pid > 0 && ((n = read(fd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)))
		if (n > 0)
			text.append(buf, static_cast<size_t>(n));
	close(fd[0]);
	int st = 1;
	if (pid > 0)
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
	if (pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
		if (batch || pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 127)
			status("%s failed: its phrases are not prepared", argv[0].c_str());
		return false;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		if (end > at)
			list.push_back(text.substr(at, end - at));
		at = end + 1;
	}
	return true;
}

/* what the assistant (-a) and Claude (-A) say, each once, in that order;
 * false if a list failed */
bool list_phrases(std::vector<std::string> &out, bool batch)
{
	std::vector<std::string> list;
	bool ok = true;

	if (opt.assistant != "-")
		ok &= run_list({ opt.assistant, "-l", voice.lang, "-P" }, list, batch);
	if (opt.claude != "-")
		ok &= run_list({ opt.claude, "-P", voice.lang }, list, batch);
	std::set<std::string> seen;
	out.clear();
	for (std::string &s : list)
		if (seen.insert(s).second)
			out.push_back(std::move(s));
	return ok;
}

void ring(const char *line);

/*
 * The ring's "busy" while phrases are prepared: the CPU is taken for
 * minutes. Again every 20 s, as lidar-ring forgets it when it restarts.
 */
std::atomic<bool> busy_shown{ false };

void show_busy(bool on)
{
	static double sent;

	if (!on) {
		if (busy_shown.exchange(false))
			ring("busy prepare off\n");
	} else if (!busy_shown.load() || uptime() - sent >= kSpeakRepeat) {
		ring("busy prepare on\n");
		sent = uptime();
		busy_shown = true;
	}
}

/* Loads the voice, then synthesizes what the queue waits for, else the
 * phrases ahead */
void synth_thread()
{
	std::unique_lock<std::mutex> lk(mu);

	while (!exists(opt.models + "/tts/model.onnx") || !read_voice()) {
		lk.unlock();
		sleep(static_cast<unsigned>(kModelInterval));
		lk.lock();
	}
	model = Model::LOADING;
	write_state();
	lk.unlock();
	double t0 = uptime();
	cache.open(voice.dir + "/cache", opt.cache_bytes);
	if (!load_voice()) {
		/* not again and again: until lidar-models installs another */
		status("cannot load the voice in %s/tts: waiting for another model", opt.models.c_str());
		lk.lock();
		model = Model::NONE;
		queue.clear();
		stop_gen++;
		write_state();
		cv.notify_all();
		lk.unlock();
		while (realpath_of(opt.models) == voice.dir)
			sleep(static_cast<unsigned>(kModelInterval));
		execv("/proc/self/exe", args);
		exit(1);
	}
	resampler.init(voice.rate);
	status("%s voice %s loaded in %.1f s, %d threads; cache %zu kB", voice.lang.c_str(), voice.name.c_str(),
	       uptime() - t0, opt.threads, cache.bytes() >> 10);
	/* the phrases in the cache count as used; the others are prepared */
	std::vector<std::string> phrases, missing;
	list_phrases(phrases, false);
	for (const std::string &p : phrases)
		if (!cache.touch(p))
			missing.push_back(p);
	if (!missing.empty())
		status("preparing %zu of %zu phrases", missing.size(), phrases.size());
	lk.lock();
	presynth = std::move(missing);
	model = Model::READY;
	write_state();
	/* the player waits for the voice and the resampler */
	cv.notify_all();

	double presynth_t0 = uptime();
	bool paused = false;

	for (;;) {
		if (std::shared_ptr<Part> job = next_waiting()) {
			unsigned want = stop_gen.load();
			std::string text = job->text;
			double took = 0;

			job->state = Part::BUSY;
			show_busy(false);
			renice(0);
			lk.unlock();
			std::shared_ptr<const Audio> a = cache.get(text);
			if (!a) {
				auto made = std::make_shared<Audio>();
				if (synthesize(text, *made, &stop_gen, want, &took)) {
					cache.put(text, *made);
					a = made;
					verbose("synthesized %zu bytes: %.2f s of audio in %.2f s", text.size(),
						static_cast<double>(made->samples.size()) / voice.rate, took);
				}
			}
			lk.lock();
			job->state = a ? Part::READY : Part::FAILED;
			job->audio = a;
			/* the same text further on */
			for (const auto &an : queue)
				for (const auto &p : an->parts)
					if (a && p->state == Part::WAITING && p->text == text) {
						p->state = Part::READY;
						p->audio = a;
					}
			cv.notify_all();
			continue;
		}
		if (presynth_next < presynth.size()) {
			/*
			 * Only while nothing else goes on: no answer queued or
			 * playing, nobody speaking to the Echo (the recognizer
			 * needs the CPU then), at the lowest priority. A phrase
			 * the model is at cannot be interrupted; an answer that
			 * needs the model raises its priority (handle).
			 */
			bool wait = !queue.empty() || speaking || exists(opt.run + "/lidar-wake.listening");
			if (wait != paused)
				verbose("preparing phrases: %s at %zu of %zu", wait ? "paused" : "goes on", presynth_next,
					presynth.size());
			paused = wait;
			if (wait) {
				show_busy(false);
				cv.wait_for(lk, std::chrono::milliseconds(250));
				continue;
			}
			std::string text = presynth[presynth_next];
			show_busy(true);
			renice(kPresynthNice);
			lk.unlock();
			Audio a;
			if (synthesize(text, a, nullptr, 0))
				cache.put(text, a);
			lk.lock();
			presynth_next++;
			write_state();
			if (presynth_next == presynth.size()) {
				show_busy(false);
				status("prepared %zu phrases in %.0f s; cache %zu kB", presynth.size(),
				       uptime() - presynth_t0, cache.bytes() >> 10);
			}
			continue;
		}
		cv.wait(lk);
	}
}

void viz_until(double seconds)
{
	std::string path = opt.run + "/lidar-viz.quiet", tmp = path + ".tts", old;
	double until = uptime() + seconds, was = 0;

	if (read_text(path, old))
		was = atof(old.c_str());
	if (was >= until)
		return;
	FILE *f = fopen(tmp.c_str(), "we");
	if (!f)
		return;
	fprintf(f, "%.2f\n", until);
	if (fclose(f) || rename(tmp.c_str(), path.c_str()))
		unlink(tmp.c_str());
}

/* a line to lidar-ring, if it runs; never waits for it */
void ring(const char *line)
{
	std::string path = opt.run + "/lidar-ring.fifo";
	struct stat st;

	if (stat(path.c_str(), &st) || !S_ISFIFO(st.st_mode))
		return;
	int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, line, strlen(line)) < 0) {
		/* the ring is not essential */
	}
	close(fd);
}

bool muted()
{
	std::string s;
	int level, mute = 0;

	return read_text(opt.run + "/lidar-volume.state", s) && sscanf(s.c_str(), "%d %d", &level, &mute) == 2 &&
	       mute == 1;
}

bool codec_closed()
{
	std::string s;

	return opt.wav.empty() && read_text(kCodecStatus, s) && s.find("closed") != std::string::npos;
}

std::string listening()
{
	std::string s;

	read_text(opt.run + "/lidar-wake.listening", s);
	return s;
}

/* Plays the queue, answer by answer, part by part */
void player_thread()
{
	player_tid = static_cast<pid_t>(syscall(SYS_gettid));
	std::unique_lock<std::mutex> lk(mu);
	std::vector<int16_t> lr;

	for (;;) {
		/* answers that come while the voice loads wait for it, even
		 * cached ones: the resampler is set up with the voice */
		cv.wait(lk, [] { return !queue.empty() && model == Model::READY; });
		/* a run of answers, until the queue is empty */
		speaking = true;
		write_state();
		std::string wake;	/* the wake word an answer follows */
		bool stopped = false, any = false;
		double last_speak = -1e9, pause = 0;

		while (!queue.empty()) {
			std::shared_ptr<Answer> a = queue.front();
			unsigned gen = stop_gen.load();
			bool answer_first = true;

			wake = listening();
			stopped = false;
			for (const auto &p : a->parts) {
				cv.wait(lk, [&] {
					return stop_gen.load() != gen || p->state == Part::READY || p->state == Part::FAILED;
				});
				if (stop_gen.load() != gen)
					break;
				if (p->pause) {
					pause = std::max(pause, kSentenceGap);
					continue;
				}
				if (!p->audio || p->audio->samples.empty() || muted())
					continue;
				std::shared_ptr<const Audio> audio = p->audio;
				double gap = std::max(pause, answer_first ? kAnswerGap : kPartGap), wake_up = 0;
				lk.unlock();
				resampler.run(audio->samples, lr);
				if (!out.is_open()) {
					wake_up = codec_closed() ? kCodecWake : 0;
					gap = 0;
				}
				if (out.is_open() || out.open()) {
					viz_until(wake_up + gap + static_cast<double>(lr.size() / 2) / kOutRate + kVizMargin);
					if (uptime() - last_speak >= kSpeakRepeat) {
						ring("wake speak\n");
						last_speak = uptime();
					}
					if (answer_first)
						verbose("answer %u: first sound %.0f ms after it came, %u of %u parts cached",
							a->id, (uptime() - a->received) * 1000, a->cached, a->texts);
					out.silence(wake_up + gap, stop_gen, gen);
					out.write(lr.data(), lr.size() / 2, stop_gen, gen);
					any = true;
					answer_first = false;
					pause = 0;
				}
				lk.lock();
				if (stop_gen.load() != gen)
					break;
			}
			if (stop_gen.load() != gen) {
				/* "stop" emptied the queue: cut off what plays at once */
				stopped = true;
				last_speak = -1e9;
				pause = 0;
				lk.unlock();
				if (out.is_open())
					out.close(false);
				lk.lock();
				continue;
			}
			queue.pop_front();
			verbose("answer %u: done", a->id);
			write_state();
		}
		lk.unlock();
		if (out.is_open())
			out.close(true);
		/* "stop" and a new wake word own the ring */
		std::string now = listening();
		if (!stopped && (now.empty() || now == wake))
			ring("wake off\n");
		lk.lock();
		speaking = false;
		write_state();
		verbose("%s", stopped ? "stopped" : any ? "all said" : "nothing to say");
	}
}

/* ---- input ---- */

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");

	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void handle(const std::string &line)
{
	static unsigned ids;

	if (line == "stop") {
		std::lock_guard<std::mutex> lk(mu);
		if (!queue.empty())
			verbose("stop: %zu answers dropped", queue.size());
		queue.clear();
		stop_gen++;
		write_state();
		cv.notify_all();
		return;
	}
	if (line.compare(0, 4, "say "))
		return;
	size_t sp = line.find(' ', 4);
	std::string lang = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
	std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
	auto a = std::make_shared<Answer>();

	a->received = uptime();
	a->id = ++ids;
	a->texts = a->cached = 0;
	for (size_t at = 0; at <= rest.size();) {
		size_t end = rest.find('|', at);
		if (end == std::string::npos)
			end = rest.size();
		std::string text = trim(rest.substr(at, end - at));
		at = end + 1;
		if (text.empty())
			continue;
		auto p = std::make_shared<Part>();
		p->text = text;
		if (text == ".") {
			p->pause = true;
			p->state = Part::READY;
		} else {
			a->texts++;
			if ((p->audio = cache.get(text))) {
				p->state = Part::READY;
				a->cached++;
			}
		}
		a->parts.push_back(p);
	}
	std::lock_guard<std::mutex> lk(mu);
	if (model == Model::NONE) {
		status("no voice installed: answer dropped");
		return;
	}
	if (lang != voice.lang) {
		status("an answer in %.8s dropped: the voice speaks %s", lang.c_str(), voice.lang.c_str());
		return;
	}
	if (!a->texts)
		return;
	verbose("answer %u: %u parts, %u cached", a->id, a->texts, a->cached);
	queue.push_back(a);
	/* a phrase being prepared stands between this answer and the model:
	 * it goes on at full speed */
	if (a->cached < a->texts)
		renice(0);
	write_state();
	cv.notify_all();
}

int open_fifo()
{
	std::string path = opt.run + "/lidar-tts.fifo";
	struct stat st;
	int fd = -1;

	/* read-write: never an end of file between writers. A regular file of
	 * that name goes. */
	for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
		if (mkfifo(path.c_str(), 0600) && errno != EEXIST)
			break;
		fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0 && (fstat(fd, &st) || !S_ISFIFO(st.st_mode))) {
			close(fd);
			fd = -1;
			if (unlink(path.c_str()))
				break;
		}
	}
	if (fd < 0)
		status("%s: %s", path.c_str(), strerror(errno));
	return fd;
}

void on_signal(int) { quit = 1; }

/* -I: phonemes, ids and timings of one text */
int try_text()
{
	if (!read_voice() || !load_voice()) {
		fprintf(stderr, "lidar-tts: cannot load the voice in %s\n", opt.models.c_str());
		return 1;
	}
	auto id_of = [&](char32_t c) {
		auto t = voice.tokens.find(c);
		return t == voice.tokens.end() ? -1 : t->second;
	};
	auto print_ids = [&](const std::string &text) {
		for (const auto &s : phonemize(text)) {
			std::string ph, ids = std::to_string(id_of(U'^')) + " " + std::to_string(id_of(U'_'));
			for (char32_t c : s) {
				const Cluster *cl = nullptr;
				for (const Cluster &k : voice.clusters)
					if (k.code == c)
						cl = &k;
				if (cl) {
					ph += "[";
					for (char32_t x : cl->phonemes)
						utf8_append(ph, x);
					ph += "]";
				} else {
					utf8_append(ph, c);
				}
				int id = id_of(c);
				char unknown[32];
				snprintf(unknown, sizeof unknown, " unknown:U+%04X", static_cast<unsigned>(c));
				/* sherpa-onnx skips what the voice does not know */
				ids += id >= 0 ? " " + std::to_string(id) + " " + std::to_string(id_of(U'_')) : unknown;
			}
			ids += " " + std::to_string(id_of(U'$'));
			printf("phonemes: %s\nids: %s\n", ph.c_str(), ids.c_str());
		}
	};
	if (opt.say_text == "-") {
		char line[4096];
		while (fgets(line, sizeof line, stdin)) {
			line[strcspn(line, "\r\n")] = 0;
			printf("text: %s\n", line);
			print_ids(line);
		}
		return 0;
	}
	print_ids(opt.say_text);
	Audio a;
	for (int run = 0; run < 2; run++) {
		double took = 0;
		if (!synthesize(opt.say_text, a, nullptr, 0, &took)) {
			fprintf(stderr, "lidar-tts: synthesis failed\n");
			return 1;
		}
		double secs = static_cast<double>(a.samples.size()) / voice.rate;
		printf("run %d: %.2f s of audio (trimmed) in %.3f s, RTF %.3f, %d threads\n", run, secs, took,
		       secs > 0 ? took / secs : 0, opt.threads);
	}
	if (!opt.wav.empty()) {
		std::vector<int16_t> lr;
		resampler.init(voice.rate);
		resampler.run(a.samples, lr);
		int fd = open(opt.wav.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0 || pwrite(fd, lr.data(), 2 * lr.size(), 44) != static_cast<ssize_t>(2 * lr.size())) {
			fprintf(stderr, "lidar-tts: %s: %s\n", opt.wav.c_str(), strerror(errno));
			return 1;
		}
		wav_header(fd, static_cast<uint32_t>(2 * lr.size()));
		close(fd);
	}
	return 0;
}

/* -B: the phrases into a cache directory, as the daemon prepares them */
int batch()
{
	std::vector<std::string> phrases;
	size_t done = 0;

	if (!read_voice() || !load_voice()) {
		fprintf(stderr, "lidar-tts: cannot load the voice in %s\n", opt.models.c_str());
		return 1;
	}
	if (!cache.open(opt.batch, opt.cache_bytes) || !list_phrases(phrases, true))
		return 1;
	double t0 = uptime(), audio = 0;
	for (const std::string &p : phrases) {
		if (cache.touch(p))
			continue;
		Audio a;
		if (!synthesize(p, a, nullptr, 0)) {
			fprintf(stderr, "lidar-tts: synthesis failed\n");
			return 1;
		}
		cache.put(p, a);
		audio += static_cast<double>(a.samples.size()) / voice.rate;
		done++;
	}
	fprintf(stderr, "lidar-tts: %s %s: %zu phrases, %zu rendered (%.0f s of audio) in %.1f s, %d threads; cache %zu kB\n",
		voice.lang.c_str(), voice.name.c_str(), phrases.size(), done, audio, uptime() - t0, opt.threads,
		cache.bytes() >> 10);
	return 0;
}

void usage()
{
	fprintf(stderr, "usage: lidar-tts [-m models] [-r run-dir] [-D pcm | -w file.wav] [-e espeak-data]\n"
			"                 [-a program] [-A program] [-t threads] [-c megabytes] [-v]\n"
			"       lidar-tts [-m models] [-e espeak-data] [-a program] [-A program] [-t threads]\n"
			"                 [-c megabytes] -B cache-dir\n"
			"       lidar-tts [-m models] [-e espeak-data] [-t threads] [-w file.wav] -I text\n");
	exit(2);
}

}  // namespace

int main(int argc, char **argv)
{
	int c;
	char *end;

	while ((c = getopt(argc, argv, "m:r:D:w:e:a:A:B:t:c:vI:")) != -1) {
		switch (c) {
		case 'm': opt.models = optarg; break;
		case 'r': opt.run = optarg; break;
		case 'D': opt.device = optarg; break;
		case 'w': opt.wav = optarg; break;
		case 'e': opt.espeak_data = optarg; break;
		case 'a': opt.assistant = optarg; break;
		case 'A': opt.claude = optarg; break;
		case 'B': opt.batch = optarg; break;
		case 't':
			opt.threads = static_cast<int>(strtol(optarg, &end, 10));
			if (*end || opt.threads < 1 || opt.threads > 64)
				usage();
			break;
		case 'c': {
			long mb = strtol(optarg, &end, 10);
			if (*end || mb < 1 || mb > 1024)
				usage();
			opt.cache_bytes = static_cast<size_t>(mb) << 20;
			break;
		}
		case 'v': opt.verbose = true; break;
		case 'I':
			opt.say_text = optarg;
			opt.test = true;
			break;
		default: usage();
		}
	}
	if (optind != argc)
		usage();
	if (opt.test)
		return try_text();
	if (!opt.batch.empty())
		return batch();
	args = argv;

	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	/* an earlier lidar-tts may have ended while preparing: lidar-ring
	 * keeps a busy reason until it is told otherwise */
	ring("busy prepare off\n");
	int fd = open_fifo();
	if (fd < 0)
		return 1;
	{
		std::lock_guard<std::mutex> lk(mu);
		write_state();
	}
	std::thread(player_thread).detach();
	std::thread(synth_thread).detach();

	std::string pending, target;
	double next_model = uptime() + kModelInterval;
	struct pollfd p = { fd, POLLIN, 0 };

	while (!quit) {
		char buf[4096];
		ssize_t n;

		if (poll(&p, 1, 1000) < 0 && errno != EINTR)
			break;
		while ((n = read(fd, buf, sizeof buf)) > 0) {
			pending.append(buf, static_cast<size_t>(n));
			for (size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
				handle(pending.substr(0, eol));
				pending.erase(0, eol + 1);
			}
			if (pending.size() > sizeof buf)
				pending.clear();	/* no line is that long */
		}
		/* idle: has lidar-models switched the language? */
		if (uptime() < next_model)
			continue;
		next_model = uptime() + kModelInterval;
		std::unique_lock<std::mutex> lk(mu);
		if (model != Model::READY || speaking || !queue.empty())
			continue;
		if (realpath_of(opt.models) != voice.dir) {
			status("%s changed, loading again", opt.models.c_str());
			show_busy(false);
			execv("/proc/self/exe", args);
			return 1;
		}
	}
	/* cut off what plays, then go */
	{
		std::lock_guard<std::mutex> lk(mu);
		queue.clear();
		stop_gen++;
		cv.notify_all();
	}
	for (int i = 0; i < 50; i++) {
		{
			std::lock_guard<std::mutex> lk(mu);
			if (!speaking)
				break;
		}
		usleep(10000);
	}
	show_busy(false);
	unlink((opt.run + "/lidar-tts.json").c_str());
	_exit(0);
}
