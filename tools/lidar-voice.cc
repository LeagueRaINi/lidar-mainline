// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-voice: the voice of whoever speaks to the Echo, from its seven
 * microphones.
 *
 *   capture   the mic array (8 TDM channels: ring mics 0-5, centre mic 6,
 *             line-in 7) and the speaker reference (the AFE's loopback of
 *             the DAC input), both at 48 kHz through the shared dsnoop
 *             devices, decimated to 16 kHz here (a windowed-sinc FIR: the
 *             phase between mics must stay intact)
 *   echo      WebRTC AEC3 on all seven mics at once (multi-channel capture),
 *             with the reference as the one render channel
 *   beams     12 fixed superdirective beams, 30 degrees apart in the plane
 *             of the array (STFT, 512 points, hop 256): for each beam and
 *             frequency the weights minimise diffuse noise for a unit gain
 *             towards the beam, with diagonal loading raised until the white
 *             noise gain stays above a floor, so errors in the geometry and
 *             the mics' own noise are not blown up at low frequencies
 *   select    the beam with the best speech-band (300-3400 Hz) level over
 *             its own slowly tracked noise floor; another beam takes over
 *             only when it is 1.5 dB better for 100 ms
 *   clean     WebRTC noise suppression (and optionally AGC2) on the beam
 *
 * Writes 16 kHz 16-bit mono to stdout, and the beam state to a JSON file
 * (-s, default /run/lidar-voice.json) ten times a second: the selected beam
 * and its angle, every beam's level and level over noise.
 *
 * Geometry, measured on the device: the six ring mics are 32 mm from the
 * centre mic (and from each other), ring mic n at 60*n degrees clockwise
 * seen from above, so ring mic 2 is the one next to the action button (mute
 * opposite, at mic 5). Beam b points at 30*b degrees in the same frame. The
 * centre mic shows no interference tones in a quiet room (within 4 dB of the
 * ring mics, 50 Hz-8 kHz); -c 0 leaves it out of the beams.
 *
 * Usage: lidar-voice [-m mic-device] [-R reference-device] [-r radius-mm]
 *                    [-b beam|-1] [-o beam|mix|centre] [-c 0|1] [-n 0-3] [-g]
 *                    [-w wng-floor-dB] [-s state-file] [-v]
 *   -b  a fixed beam (0-11); -1 (default) selects automatically
 *   -o  beam (default); mix: the echo-cancelled ring mics averaged, without
 *       beams; centre: the centre mic alone (both for comparison)
 *   -c  1 (default): the beams use the centre mic too; 0: the ring only
 *   -n  noise suppression level (0 off, 1 low, 2 moderate, 3 high), default 2
 *   -g  adaptive digital gain (AGC2)
 *   -w  white noise gain floor in dB (-30 to 8), default -5
 *   -v  levels, beam and echo canceller statistics on stderr once a second
 */
#include <alsa/asoundlib.h>
#include <api/audio/audio_processing.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using cf = std::complex<float>;
using cd = std::complex<double>;

constexpr int kInRate = 48000;
constexpr int kRate = 16000;
constexpr int kDecim = kInRate / kRate;
constexpr snd_pcm_uframes_t kInFrames = kInRate / 100;	// 10 ms
constexpr int kFrames = kRate / 100;			// 10 ms, what APM takes
constexpr int kMicChannels = 8;
constexpr int kMics = 7;				// ring 0-5, centre 6
constexpr int kRing = 6;
constexpr int kRefChannels = 2;
/*
 * The echo reaches the mics only 2.4 ms after the reference, less than
 * AEC3's delay estimator expects: without a margin it locks onto a wrong
 * delay and its linear filter never converges (measured on this device: 5-13
 * dB of suppression instead of ~40 dB). The mics run 40 ms behind.
 */
constexpr int kMicDelayPeriods = 4;
constexpr int kN = 512;			// STFT size
constexpr int kHop = kN / 2;
constexpr int kBins = kN / 2 + 1;
constexpr int kBeams = 12;
constexpr double kC = 343.0;		// speed of sound, m/s
constexpr int kTaps = 95;		// decimation filter

volatile sig_atomic_t quit;

/* ---- ALSA ---- */

snd_pcm_t *open_capture(const char *name, snd_pcm_format_t format, unsigned int channels)
{
	snd_pcm_t *pcm;
	int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0);

	if (err < 0) {
		fprintf(stderr, "lidar-voice: %s: %s\n", name, snd_strerror(err));
		return nullptr;
	}
	/* 80 ms: what the shared device holds (8 periods of 10 ms) */
	err = snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED, channels,
				 kInRate, 0, 80000);
	if (err < 0) {
		fprintf(stderr, "lidar-voice: %s: %s\n", name, snd_strerror(err));
		snd_pcm_close(pcm);
		return nullptr;
	}
	return pcm;
}

enum class Read { ok, xrun, gone };

/* One 10 ms period from the mics, which set the pace. */
Read read_period(snd_pcm_t *pcm, void *buf, size_t frame_bytes)
{
	auto *p = static_cast<char *>(buf);
	snd_pcm_uframes_t left = kInFrames;

	while (left) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, p, left);

		if (n == -EAGAIN)
			continue;
		if (n == -EINTR) {
			if (quit)
				return Read::gone;
			continue;
		}
		if (n == -EPIPE || n == -ESTRPIPE)
			return Read::xrun;
		if (n < 0) {
			fprintf(stderr, "lidar-voice: capture: %s\n", snd_strerror(n));
			return Read::gone;
		}
		p += n * frame_bytes;
		left -= n;
	}
	return Read::ok;
}

/*
 * One period of the reference, without waiting: the loopback delivers
 * nothing while the speaker output is closed, and the reference is silence
 * then. A stream that overran or stopped is started again.
 */
void read_reference(snd_pcm_t *ref, int16_t *buf)
{
	snd_pcm_sframes_t av = snd_pcm_avail_update(ref);

	if (av < 0) {
		snd_pcm_recover(ref, static_cast<int>(av), 1);
		av = 0;
	}
	if (snd_pcm_state(ref) == SND_PCM_STATE_PREPARED)
		snd_pcm_start(ref);
	if (av >= static_cast<snd_pcm_sframes_t>(kInFrames)) {
		snd_pcm_sframes_t n = snd_pcm_readi(ref, buf, kInFrames);

		if (n == static_cast<snd_pcm_sframes_t>(kInFrames))
			return;
		if (n < 0)
			snd_pcm_recover(ref, static_cast<int>(n), 1);
	}
	std::fill(buf, buf + kInFrames * kRefChannels, 0);
}

bool write_all(const int16_t *buf, size_t n)
{
	auto *p = reinterpret_cast<const char *>(buf);
	size_t left = n * sizeof(int16_t);

	while (left) {
		ssize_t w = write(1, p, left);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		left -= static_cast<size_t>(w);
	}
	return true;
}

/* ---- 48 -> 16 kHz: windowed-sinc low-pass at 6.8 kHz, every third output ---- */

struct Decimator {
	std::array<float, kTaps> h{};
	std::vector<float> hist;	/* the last kTaps - 1 inputs, then this period's */

	Decimator()
	{
		const double fc = 6800.0 / kInRate;	/* cycles per sample */
		double sum = 0;

		for (int i = 0; i < kTaps; i++) {
			double t = i - (kTaps - 1) / 2.0;
			double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
			double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1)) +
				   0.08 * std::cos(4 * M_PI * i / (kTaps - 1));	/* Blackman */

			h[i] = static_cast<float>(sinc * w);
			sum += sinc * w;
		}
		for (auto &v : h)
			v = static_cast<float>(v / sum);
		hist.assign(kTaps - 1 + kInFrames, 0.0f);
	}

	/* in: kInFrames samples; out: kFrames samples */
	void run(const float *in, float *out)
	{
		std::copy(in, in + kInFrames, hist.begin() + (kTaps - 1));
		for (int o = 0; o < kFrames; o++) {
			const float *x = &hist[o * kDecim];
			float acc = 0;

			for (int i = 0; i < kTaps; i++)
				acc += h[i] * x[kTaps - 1 - i];
			out[o] = acc;
		}
		std::copy(hist.end() - (kTaps - 1), hist.end(), hist.begin());
	}
};

/* ---- FFT (radix 2, in place) ---- */

void fft(cf *x, int n, bool inverse)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(x[i], x[j]);
	}
	for (int len = 2; len <= n; len <<= 1) {
		double a = (inverse ? 2 : -2) * M_PI / len;
		cf wl(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));

		for (int i = 0; i < n; i += len) {
			cf w(1, 0);

			for (int k = 0; k < len / 2; k++) {
				cf u = x[i + k], v = x[i + k + len / 2] * w;

				x[i + k] = u + v;
				x[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
	if (inverse)
		for (int i = 0; i < n; i++)
			x[i] /= static_cast<float>(n);
}

/* ---- beam design ---- */

/* x = A^-1 b for a small complex system (Gaussian elimination, pivoting) */
bool solve(std::vector<cd> a, std::vector<cd> b, int n, std::vector<cd> &x)
{
	for (int c = 0; c < n; c++) {
		int p = c;

		for (int r = c + 1; r < n; r++)
			if (std::abs(a[r * n + c]) > std::abs(a[p * n + c]))
				p = r;
		if (std::abs(a[p * n + c]) < 1e-15)
			return false;
		if (p != c) {
			for (int k = 0; k < n; k++)
				std::swap(a[c * n + k], a[p * n + k]);
			std::swap(b[c], b[p]);
		}
		for (int r = c + 1; r < n; r++) {
			cd f = a[r * n + c] / a[c * n + c];

			for (int k = c; k < n; k++)
				a[r * n + k] -= f * a[c * n + k];
			b[r] -= f * b[c];
		}
	}
	x.assign(n, 0);
	for (int r = n - 1; r >= 0; r--) {
		cd s = b[r];

		for (int k = r + 1; k < n; k++)
			s -= a[r * n + k] * x[k];
		x[r] = s / a[r * n + r];
	}
	return true;
}

struct Beams {
	/* w[beam][bin][mic], applied as y = sum conj(w) x */
	std::vector<cf> w;

	cf &at(int b, int k, int m) { return w[(b * kBins + k) * kMics + m]; }

	/* nm = 6: the ring mics only; 7: with the centre mic */
	void design(double radius, double wng_floor_db, int nm)
	{
		double px[kMics], py[kMics];

		for (int m = 0; m < kRing; m++) {
			double phi = m * M_PI / 3;	/* 60 degrees apart */

			px[m] = radius * std::cos(phi);
			py[m] = radius * std::sin(phi);
		}
		px[kRing] = py[kRing] = 0;
		w.assign(static_cast<size_t>(kBeams) * kBins * kMics, cf(0, 0));
		const double wng_floor = std::pow(10.0, wng_floor_db / 10);

		for (int k = 0; k < kBins; k++) {
			double f = k * static_cast<double>(kRate) / kN;
			std::vector<cd> gamma(nm * nm);

			/* diffuse (spherically isotropic) noise coherence */
			for (int i = 0; i < nm; i++)
				for (int j = 0; j < nm; j++) {
					double d = std::hypot(px[i] - px[j], py[i] - py[j]);
					double x = 2 * M_PI * f * d / kC;

					gamma[i * nm + j] = x < 1e-9 ? 1.0 : std::sin(x) / x;
				}
			for (int b = 0; b < kBeams; b++) {
				double th = b * M_PI / 6;	/* 30 degrees apart */
				std::vector<cd> d(nm), x;

				/* a plane wave from th reaches mics facing it first */
				for (int m = 0; m < nm; m++) {
					double tau = -(px[m] * std::cos(th) + py[m] * std::sin(th)) / kC;

					d[m] = std::polar(1.0, -2 * M_PI * f * tau);
				}
				for (double mu = 1e-4;; mu *= 1.5) {
					std::vector<cd> r = gamma;

					for (int i = 0; i < nm; i++)
						r[i * nm + i] += mu;
					if (!solve(r, d, nm, x))
						continue;
					cd den = 0;	/* d^H R^-1 d */
					for (int m = 0; m < nm; m++)
						den += std::conj(d[m]) * x[m];
					double norm = 0;
					for (int m = 0; m < nm; m++) {
						x[m] /= den;	/* w^H d = 1 */
						norm += std::norm(x[m]);
					}
					/* white noise gain = |w^H d|^2 / w^H w = 1 / w^H w */
					if (1 / norm >= wng_floor || mu > 1e3) {
						for (int m = 0; m < nm; m++)
							at(b, k, m) = cf(static_cast<float>(x[m].real()),
									 static_cast<float>(x[m].imag()));
						break;
					}
				}
			}
		}
	}
};

/* ---- beam selection ---- */

/*
 * Each beam's noise floor is the minimum of its smoothed level over the last
 * ~3 s (12 sub-windows of 16 hops): speech comes and goes, the floor under
 * it does not. The first 0.5 s (AEC settling, empty STFT frames) count for
 * nothing.
 */
constexpr int kSub = 12, kSubHops = 16, kWarmHops = 32;

struct Selector {
	std::array<double, kBeams> level{}, floor{}, snr{}, cur_min{};
	std::array<std::array<double, kSub>, kBeams> mins{};
	int current = 0, rival = -1, held = 0, sub = 0;
	unsigned long hop = 0;

	/* one hop of per-beam speech-band power; returns the beam to use */
	int update(const std::array<double, kBeams> &power)
	{
		const double a = 1 - std::exp(-kHop / (0.08 * kRate));		/* ~80 ms */

		if (hop < kWarmHops) {
			if (++hop == kWarmHops)
				for (int b = 0; b < kBeams; b++) {
					level[b] = cur_min[b] = power[b] + 1e-12;
					mins[b].fill(1e30);
				}
			return current;
		}
		for (int b = 0; b < kBeams; b++) {
			level[b] += a * (power[b] + 1e-12 - level[b]);
			cur_min[b] = std::min(cur_min[b], level[b]);
			floor[b] = cur_min[b];
			for (double m : mins[b])
				floor[b] = std::min(floor[b], m);
			snr[b] = 10 * std::log10(level[b] / floor[b]);
		}
		if (++hop % kSubHops == 0) {		/* next sub-window */
			for (int b = 0; b < kBeams; b++) {
				mins[b][sub] = cur_min[b];
				cur_min[b] = level[b];
			}
			sub = (sub + 1) % kSub;
		}
		int best = static_cast<int>(std::max_element(snr.begin(), snr.end()) - snr.begin());

		if (best == current || snr[best] < snr[current] + 1.5 || snr[best] < 3) {
			rival = -1;
			held = 0;
		} else if (best != rival) {
			rival = best;
			held = 1;
		} else if (++held * kHop >= kRate / 10) {	/* 100 ms */
			current = best;
			rival = -1;
			held = 0;
		}
		return current;
	}
};

double dbfs(double power, double samples)
{
	return 10 * std::log10(power / samples + 1e-12);
}

void write_state(const std::string &path, int beam, const Selector &s, bool fixed)
{
	std::string tmp = path + ".new";
	FILE *f = fopen(tmp.c_str(), "w");

	if (!f)
		return;
	fprintf(f, "{\"beam\": %d, \"angle\": %d, \"fixed\": %s, \"level\": [", beam, beam * 30,
		fixed ? "true" : "false");
	for (int b = 0; b < kBeams; b++)
		fprintf(f, "%s%.1f", b ? ", " : "", 10 * std::log10(s.level[b] + 1e-12));
	fprintf(f, "], \"snr\": [");
	for (int b = 0; b < kBeams; b++)
		fprintf(f, "%s%.1f", b ? ", " : "", s.snr[b]);
	fprintf(f, "]}\n");
	if (fclose(f) == 0)
		rename(tmp.c_str(), path.c_str());
}

void on_signal(int) { quit = 1; }

}  // namespace

int main(int argc, char **argv)
{
	const char *mic_dev = "lidar_mic8", *ref_dev = "lidar_ref";
	std::string state = "/run/lidar-voice.json", mode = "beam";
	double radius_mm = 32, wng_db = -5;
	int fixed_beam = -1, ns_level = 2, centre = 1, opt;
	bool agc = false, verbose = false;

	while ((opt = getopt(argc, argv, "m:R:r:b:o:n:gw:c:s:v")) != -1) {
		switch (opt) {
		case 'm': mic_dev = optarg; break;
		case 'R': ref_dev = optarg; break;
		case 'r': radius_mm = atof(optarg); break;
		case 'b': fixed_beam = atoi(optarg); break;
		case 'o': mode = optarg; break;
		case 'n': ns_level = atoi(optarg); break;
		case 'g': agc = true; break;
		case 'w': wng_db = atof(optarg); break;
		case 'c': centre = atoi(optarg); break;
		case 's': state = optarg; break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: lidar-voice [-m mic] [-R reference] [-r radius-mm] [-b beam|-1] "
					"[-o beam|mix|centre] [-c 0|1] [-n 0-3] [-g] [-w wng-dB] [-s state] [-v]\n");
			return 2;
		}
	}
	if (ns_level < 0 || ns_level > 3 || fixed_beam < -1 || fixed_beam >= kBeams ||
	    radius_mm < 10 || radius_mm > 100 || wng_db < -30 || wng_db > 8 ||
	    (centre != 0 && centre != 1) ||
	    (mode != "beam" && mode != "mix" && mode != "centre") || (mode == "centre" && !centre)) {
		fprintf(stderr, "lidar-voice: bad option\n");
		return 2;
	}
	const int nm = centre ? kMics : kRing;	/* mics the beams use */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	Beams beams;
	beams.design(radius_mm / 1000, wng_db, nm);

	snd_pcm_t *mic = open_capture(mic_dev, SND_PCM_FORMAT_S32_LE, kMicChannels);
	snd_pcm_t *ref = open_capture(ref_dev, SND_PCM_FORMAT_S16_LE, kRefChannels);

	if (!mic || !ref)
		return 1;
	snd_pcm_nonblock(ref, 1);

	/* echo cancellation on the mics at once */
	webrtc::AudioProcessing::Config aec_cfg;
	aec_cfg.echo_canceller.enabled = true;
	aec_cfg.high_pass_filter.enabled = true;
	aec_cfg.pipeline.multi_channel_capture = true;
	aec_cfg.pipeline.multi_channel_render = false;
	rtc::scoped_refptr<webrtc::AudioProcessing> aec = webrtc::AudioProcessingBuilder().Create();
	aec->ApplyConfig(aec_cfg);

	/* the beam's clean-up */
	webrtc::AudioProcessing::Config out_cfg;
	out_cfg.noise_suppression.enabled = ns_level > 0;
	if (ns_level > 0)
		out_cfg.noise_suppression.level =
			static_cast<webrtc::AudioProcessing::Config::NoiseSuppression::Level>(ns_level - 1);
	out_cfg.gain_controller2.enabled = agc;
	out_cfg.gain_controller2.adaptive_digital.enabled = agc;
	rtc::scoped_refptr<webrtc::AudioProcessing> post = webrtc::AudioProcessingBuilder().Create();
	post->ApplyConfig(out_cfg);

	const webrtc::StreamConfig mics_cfg(kRate, nm), mono_cfg(kRate, 1);
	std::vector<int32_t> mic_buf(kInFrames * kMicChannels);
	std::vector<int16_t> ref_buf(kInFrames * kRefChannels);
	std::vector<float> in48(kInFrames), fresh(kFrames);
	std::array<Decimator, kMics + 1> dec;		/* 7 mics + reference */
	std::vector<std::vector<float>> ch(kMics + 1, std::vector<float>(kFrames));
	std::vector<std::vector<float>> delay(kMics, std::vector<float>(kMicDelayPeriods * kFrames, 0.0f));
	float *mic_ptr[kMics], *ref_ptr[1] = { ch[kMics].data() };

	for (int m = 0; m < kMics; m++)
		mic_ptr[m] = ch[m].data();

	/* STFT state */
	std::vector<float> win(kN);
	for (int i = 0; i < kN; i++)
		win[i] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2 * M_PI * i / kN)));	/* sqrt periodic Hann */
	std::vector<std::vector<float>> frame(kMics, std::vector<float>(kN, 0.0f));
	std::vector<std::vector<float>> acc(kMics);
	std::vector<float> ola(kN, 0.0f), outq;
	std::vector<std::vector<cf>> spec(kMics, std::vector<cf>(kN));
	std::vector<cf> y(kN);
	Selector sel;
	int beam = fixed_beam >= 0 ? fixed_beam : 0, last_beam = beam, rc = 0;
	const int lo = 300 * kN / kRate, hi = 3400 * kN / kRate;
	bool apm_err = false;

	std::vector<int16_t> out(kFrames);
	std::vector<float> clean(kFrames);
	float *clean_ptr[] = { clean.data() };
	double ref_pow = 0, mic_pow = 0, out_pow = 0;
	unsigned long hops = 0;

	/* after an overrun the mic history no longer joins up: start over */
	auto reset = [&]() {
		for (auto &d : delay)
			std::fill(d.begin(), d.end(), 0.0f);
		for (auto &d : dec)
			d = Decimator();
		for (int m = 0; m < kMics; m++) {
			std::fill(frame[m].begin(), frame[m].end(), 0.0f);
			acc[m].clear();
		}
	};

	if (snd_pcm_start(mic) < 0) {
		fprintf(stderr, "lidar-voice: cannot start the mic capture\n");
		rc = 1;
	}
	for (unsigned long period = 1; !quit && rc == 0; period++) {
		Read r = read_period(mic, mic_buf.data(), kMicChannels * sizeof(int32_t));

		if (r == Read::xrun) {
			fprintf(stderr, "lidar-voice: overrun, restarting capture\n");
			reset();
			snd_pcm_drop(mic);
			if (snd_pcm_prepare(mic) < 0 || snd_pcm_start(mic) < 0)
				rc = 1;
			continue;
		}
		if (r == Read::gone) {
			rc = quit ? 0 : 1;
			break;
		}
		read_reference(ref, ref_buf.data());

		/* 48 -> 16 kHz, reference as the mono mix the speaker plays */
		for (snd_pcm_uframes_t i = 0; i < kInFrames; i++)
			in48[i] = (ref_buf[2 * i] + ref_buf[2 * i + 1]) / 65536.0f;
		dec[kMics].run(in48.data(), ch[kMics].data());
		for (float v : ch[kMics])
			ref_pow += v * v;
		for (int m = 0; m < kMics; m++) {
			for (snd_pcm_uframes_t i = 0; i < kInFrames; i++)
				in48[i] = mic_buf[i * kMicChannels + m] / 2147483648.0f;
			std::vector<float> &dl = delay[m];
			dec[m].run(in48.data(), fresh.data());
			/* the mics run 40 ms behind the reference (kMicDelayPeriods) */
			std::copy_n(dl.begin(), kFrames, ch[m].begin());
			std::rotate(dl.begin(), dl.begin() + kFrames, dl.end());
			std::copy(fresh.begin(), fresh.end(), dl.end() - kFrames);
		}
		for (float v : ch[0])
			mic_pow += v * v;

		if (aec->ProcessReverseStream(ref_ptr, mono_cfg, mono_cfg, ref_ptr) != 0 ||
		    aec->ProcessStream(mic_ptr, mics_cfg, mics_cfg, mic_ptr) != 0) {
			if (!apm_err)
				fprintf(stderr, "lidar-voice: echo canceller rejected the audio\n");
			apm_err = true;
		}

		for (int m = 0; m < kMics; m++)
			acc[m].insert(acc[m].end(), ch[m].begin(), ch[m].end());

		/* every hop of 256 samples: STFT, beams, selection, overlap-add */
		while (static_cast<int>(acc[0].size()) >= kHop) {
			for (int m = 0; m < kMics; m++) {
				std::vector<float> &fr = frame[m];

				std::copy(fr.begin() + kHop, fr.end(), fr.begin());
				std::copy_n(acc[m].begin(), kHop, fr.end() - kHop);
				acc[m].erase(acc[m].begin(), acc[m].begin() + kHop);
				if (m >= nm && !(mode == "centre" && m == kRing))
					continue;
				for (int i = 0; i < kN; i++)
					spec[m][i] = cf(fr[i] * win[i], 0);
				fft(spec[m].data(), kN, false);
			}
			std::array<double, kBeams> power{};

			for (int b = 0; b < kBeams; b++)
				for (int k = lo; k <= hi; k++) {
					cf s(0, 0);

					for (int m = 0; m < nm; m++)
						s += std::conj(beams.at(b, k, m)) * spec[m][k];
					power[b] += std::norm(s);
				}
			if (fixed_beam < 0)
				beam = sel.update(power);
			else
				sel.update(power);

			for (int k = 0; k < kBins; k++) {
				cf s(0, 0);

				if (mode == "beam") {
					for (int m = 0; m < nm; m++)
						s += std::conj(beams.at(beam, k, m)) * spec[m][k];
					if (beam != last_beam) {	/* half each across a switch */
						cf o(0, 0);

						for (int m = 0; m < nm; m++)
							o += std::conj(beams.at(last_beam, k, m)) * spec[m][k];
						s = 0.5f * (s + o);
					}
				} else if (mode == "mix") {
					for (int m = 0; m < kRing; m++)
						s += spec[m][k];
					s /= static_cast<float>(kRing);
				} else {
					s = spec[kRing][k];
				}
				y[k] = s;
				if (k > 0 && k < kN / 2)
					y[kN - k] = std::conj(s);
			}
			last_beam = beam;
			fft(y.data(), kN, true);
			for (int i = 0; i < kN; i++)
				ola[i] += y[i].real() * win[i];
			outq.insert(outq.end(), ola.begin(), ola.begin() + kHop);
			std::copy(ola.begin() + kHop, ola.end(), ola.begin());
			std::fill(ola.end() - kHop, ola.end(), 0.0f);
			if (++hops % 6 == 0)		/* ~100 ms */
				write_state(state, beam, sel, fixed_beam >= 0);
		}

		/* clean-up and output in 10 ms frames */
		while (static_cast<int>(outq.size()) >= kFrames) {
			std::copy_n(outq.begin(), kFrames, clean.begin());
			outq.erase(outq.begin(), outq.begin() + kFrames);
			if (post->ProcessStream(clean_ptr, mono_cfg, mono_cfg, clean_ptr) != 0 && !apm_err) {
				fprintf(stderr, "lidar-voice: noise suppression rejected the audio\n");
				apm_err = true;
			}
			for (int i = 0; i < kFrames; i++) {
				out[i] = static_cast<int16_t>(std::clamp(clean[i], -1.0f, 1.0f) * 32767.0f);
				out_pow += clean[i] * clean[i];
			}
			if (!write_all(out.data(), out.size()))
				quit = 1;
		}

		if (verbose && period % 100 == 0) {
			webrtc::AudioProcessingStats s = aec->GetStatistics();
			const double n = 100.0 * kFrames;

			fprintf(stderr, "%lus ref %.1f mic0 %.1f out %.1f dBFS, beam %d (%d deg, %.1f dB over noise), "
					"delay %d ms, ERLE %.1f dB\n",
				period / 100, dbfs(ref_pow, n), dbfs(mic_pow, n), dbfs(out_pow, n),
				beam, beam * 30, sel.snr[beam], s.delay_ms.value_or(-1),
				s.echo_return_loss_enhancement.value_or(0));
			ref_pow = mic_pow = out_pow = 0;
		}
	}
	unlink(state.c_str());
	snd_pcm_close(mic);
	snd_pcm_close(ref);
	return rc;
}
