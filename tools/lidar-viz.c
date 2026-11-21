// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-viz: the LED ring as a sound display (visualizer service).
 *
 * Listens to what the speaker plays: the AFE's loopback of the DAC input
 * (hw:amazonlidar,2), so every source counts (Bluetooth, the web player, the
 * USB host, system sounds). 40 frames per second.
 *
 * The loopback is taken after the speaker tuning and before the codec. With
 * the tuning (lidar-dsp ready) the system volume is already in it up to the
 * cap, which lidar-dsp applies; on the direct path the codec applies all of
 * it, after the loopback. lidar-viz adds what is missing, from
 * /run/lidar-volume.state, so both paths look the same.
 *
 *   level     the level at the speaker: an arc from LED 1 down both sides,
 *             green, yellow, red at the end, with a peak mark that falls
 *             back; longer with louder music and a higher volume
 *   spectrum  a 2048-point FFT, 7 bands from 50 Hz to 10 kHz, mirrored:
 *             bass on LED 1, treble on LED 7 opposite, red to violet; each
 *             band against its own reference level, the same at any volume
 *
 * It keeps off the ring while lidar-ring shows something itself
 * (/run/lidar-ring.busy: privacy red, volume arc, pairing) and takes that
 * service's brightness (/run/lidar-ring.level, percent, from the light
 * sensor). The ring stays dark while the speaker is muted or at volume 0
 * and while nothing plays. While lidar-usbaudio's mic pipe runs (the USB
 * host records, /run/lidar-usbaudio.mic), lidar-aec may need the loopback:
 * lidar-viz lets go of it and waits, also with "usbaudio aec off".
 *
 * Usage: lidar-viz [-s spectrum|level] [-d capture-device]
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE	48000
#define HOP	1200		/* 25 ms: 40 frames per second */
#define FPS	(RATE / HOP)
#define NFFT	2048
#define N	12		/* LEDs */
#define BANDS	7		/* spectrum bands: LED 1, 2/12, ... 7 */
#define VMAX	160		/* full LED value, as lidar-ring's colours */
#define SILENT_DB	-50.0f	/* below this, before the volume, is silence */

#define BUSY	"/run/lidar-ring.busy"
#define LEVEL	"/run/lidar-ring.level"
#define VOLUME	"/run/lidar-volume.state"
#define DSP	"/run/lidar-dsp/ready"
#define AEC	"/run/lidar-usbaudio.mic"

static volatile sig_atomic_t quit;
static int kmsg = -1;
static int led_fd[N * 3];
static int led_val[N * 3];	/* last written value, -1: unknown */

static void say(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	int n = snprintf(buf, sizeof(buf), "lidar-viz: ");

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	if (write(kmsg >= 0 ? kmsg : 2, buf, n) < 0) {
		/* nowhere else to report it */
	}
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

/* up to n integers from a file: how many were read, -1 if there is no file */
static int read_ints(const char *path, int *v, int n)
{
	FILE *f = fopen(path, "r");
	int got = 0;

	if (!f)
		return -1;
	while (got < n && fscanf(f, "%d", &v[got]) == 1)
		got++;
	fclose(f);
	return got;
}

/* ---- LEDs ---- */

static int leds_open(void)
{
	static const char *col[3] = { "red", "green", "blue" };
	char path[64];

	for (int i = 0; i < N * 3; i++) {
		snprintf(path, sizeof(path), "/sys/class/leds/ring%d:%s/brightness",
			 i / 3 + 1, col[i % 3]);
		led_fd[i] = open(path, O_WRONLY);
		if (led_fd[i] < 0) {
			say("%s: %s\n", path, strerror(errno));
			return -1;
		}
		led_val[i] = -1;
	}
	return 0;
}

static void leds_forget(void)
{
	for (int i = 0; i < N * 3; i++)
		led_val[i] = -1;
}

/*
 * Whether lidar-viz may draw now, asked right before every write: not while
 * lidar-ring holds the ring. When it hands the ring back it has cleared it,
 * so the value cache no longer matches the LEDs.
 */
static int mine(void)
{
	static int busy;

	if (exists(BUSY)) {
		busy = 1;
		return 0;
	}
	if (busy) {
		busy = 0;
		leds_forget();
	}
	return 1;
}

/* rgb 0-1 per LED, scaled to VMAX and the brightness in percent */
static void leds_show(float rgb[N][3], int percent)
{
	for (int i = 0; i < N * 3; i++) {
		float x = rgb[i / 3][i % 3];
		int v = x < 0.02f ? 0 : (int)lroundf(x * VMAX * percent / 100);
		char buf[8];

		if (x >= 0.02f && v < 1)
			v = 1;
		if (v == led_val[i])
			continue;
		int n = snprintf(buf, sizeof(buf), "%d", v);
		if (pwrite(led_fd[i], buf, n, 0) == n)
			led_val[i] = v;
	}
}

static void leds_dark(int percent)
{
	float off[N][3] = { { 0 } };

	leds_show(off, percent);
}

/* ---- analysis ---- */

/* in-place radix-2 FFT of re/im, NFFT points */
static void fft(float *re, float *im)
{
	for (int i = 1, j = 0; i < NFFT; i++) {
		int bit = NFFT >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= NFFT; len <<= 1) {
		float a = -2 * (float)M_PI / len;
		float wr = cosf(a), wi = sinf(a);

		for (int i = 0; i < NFFT; i += len) {
			float cr = 1, ci = 0;

			for (int k = 0; k < len / 2; k++) {
				float *ur = &re[i + k], *ui = &im[i + k];
				float *vr = &re[i + k + len / 2], *vi = &im[i + k + len / 2];
				float tr = *vr * cr - *vi * ci, ti = *vr * ci + *vi * cr;

				*vr = *ur - tr; *vi = *ui - ti;
				*ur += tr; *ui += ti;
				float n = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = n;
			}
		}
	}
}

static void hsv(float h, float s, float v, float out[3])
{
	static const int n[3] = { 5, 3, 1 };

	for (int c = 0; c < 3; c++) {
		float k = fmodf(n[c] + h / 60, 6);
		float m = fminf(fminf(k, 4 - k), 1);

		out[c] = v - v * s * fmaxf(0, m);
	}
}

static float smooth(float old, float v, float up, float down)
{
	return old + (v - old) * (v > old ? up : down);
}

struct state {
	float win[NFFT];		/* Hann window */
	float hist[NFFT];		/* last NFFT mono samples */
	float power[NFFT / 2];
	float band[BANDS], band_ref[BANDS];
	int band_lo[BANDS], band_hi[BANDS];
	float lvl, peak;
	int peak_hold;
};

static void analysis_init(struct state *s)
{
	const float hz = (float)RATE / NFFT;

	memset(s, 0, sizeof(*s));
	for (int i = 0; i < NFFT; i++)
		s->win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (NFFT - 1));
	/* 7 log-spaced bands, 50 Hz to 10 kHz */
	for (int b = 0; b < BANDS; b++) {
		float lo = 50 * powf(200, (float)b / BANDS);
		float hi = 50 * powf(200, (float)(b + 1) / BANDS);

		s->band_lo[b] = (int)fmaxf(1, lroundf(lo / hz));
		s->band_hi[b] = (int)lroundf(hi / hz);
		s->band_ref[b] = 1e-9f;
	}
}

/* the reference level halves in 4 s */
#define AGC_DECAY 0.99568f	/* 0.5^(1 / (FPS * 4)) at 40 frames/s */

static void spectrum(struct state *s, int silent, float out[N][3])
{
	static float re[NFFT], im[NFFT];

	for (int i = 0; i < NFFT; i++) {
		re[i] = s->hist[i] * s->win[i];
		im[i] = 0;
	}
	fft(re, im);
	for (int k = 0; k < NFFT / 2; k++)
		s->power[k] = re[k] * re[k] + im[k] * im[k];

	for (int b = 0; b < BANDS; b++) {
		float e = 0, v = 0, c[3];

		for (int k = s->band_lo[b]; k <= s->band_hi[b]; k++)
			e += s->power[k];
		s->band_ref[b] = fmaxf(fmaxf(e, s->band_ref[b] * AGC_DECAY), 1e-9f);
		/* 30 dB below the reference is dark */
		if (!silent)
			v = fmaxf(0, 1 + 10 * log10f(e / s->band_ref[b]) / 30);
		s->band[b] = smooth(s->band[b], v, 0.6f, 0.12f);
		hsv(b * 45.0f, 1, s->band[b] * s->band[b], c);
		memcpy(out[b], c, sizeof(c));
		if (b > 0 && b < BANDS - 1)
			memcpy(out[N - b], c, sizeof(c));
	}
}

/*
 * Level: the level at the speaker (the signal after the system volume) plus
 * half the volume's gain once more, LVL_LO to LVL_HI dB over the 7
 * positions. Louder music and a higher volume both make the arc longer, and
 * it gets short quickly as the volume goes down.
 */
#define LVL_LO		-45.0f
#define LVL_HI		-6.0f
#define VOL_EXTRA	0.5f

static void level(struct state *s, float speaker_db, float vol_db, int silent, float out[N][3])
{
	static const float col[3][3] = {
		{ 0.1f, 1, 0.2f }, { 1, 0.75f, 0 }, { 1, 0.1f, 0.05f },
	};
	float pos = 0;

	if (!silent) {
		float db = speaker_db + VOL_EXTRA * vol_db;

		pos = fminf(1, fmaxf(0, (db - LVL_LO) / (LVL_HI - LVL_LO))) * 7;
	}
	s->lvl = smooth(s->lvl, pos, 0.7f, 0.15f);
	if (s->lvl >= s->peak) {
		s->peak = s->lvl;
		s->peak_hold = FPS * 6 / 10;
	} else if (s->peak_hold > 0) {
		s->peak_hold--;
	} else {
		s->peak = fmaxf(0, s->peak - 7.0f / (FPS * 1.2f));
	}
	memset(out, 0, sizeof(float) * N * 3);
	for (int p = 0; p < 7; p++) {
		const float *c = col[p < 4 ? 0 : p < 6 ? 1 : 2];
		float fill = fminf(1, fmaxf(0, s->lvl - p));

		if (!silent && (int)s->peak == p && s->peak > 0.5f)
			fill = fmaxf(fill, 0.9f);
		for (int i = 0; i < 3; i++) {
			out[p][i] = c[i] * fill;
			if (p > 0 && p < 6)
				out[N - p][i] = c[i] * fill;
		}
	}
}

/* ---- capture ---- */

static snd_pcm_t *capture_open(const char *dev)
{
	snd_pcm_t *pcm;
	int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);

	if (err < 0)
		return NULL;
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				 2, RATE, 0, 100000);
	if (err < 0) {
		say("%s: %s\n", dev, snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

static void on_signal(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
	const char *dev = "hw:amazonlidar,2";
	int style_level = 0, opt;

	while ((opt = getopt(argc, argv, "s:d:")) != -1) {
		switch (opt) {
		case 's':
			if (!strcmp(optarg, "level"))
				style_level = 1;
			else if (strcmp(optarg, "spectrum"))
				goto usage;
			break;
		case 'd': dev = optarg; break;
		default:
usage:
			fprintf(stderr, "usage: lidar-viz [-s spectrum|level] [-d capture-device]\n");
			return 2;
		}
	}
	kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (leds_open() < 0)
		return 1;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	static struct state st;
	static int16_t buf[HOP * 2];
	snd_pcm_t *pcm = NULL;
	int percent = 100, dark = 0, tick = 0, said_busy = 0;
	float vol_db = 0, in_loop_db = 0;

	analysis_init(&st);
	while (!quit) {
		/* every half second: brightness, mute, volume. A file being
		 * rewritten (nothing to parse yet) keeps the last values. */
		if (tick++ % (FPS / 2) == 0) {
			int v[3], r = read_ints(LEVEL, v, 1);

			if (r == 1 && v[0] > 0 && v[0] <= 100)
				percent = v[0];
			else if (r < 0)
				percent = 100;		/* no ring service */
			r = read_ints(VOLUME, v, 3);
			if (r == 3) {
				dark = v[0] == 0 || v[1] == 1;
				/* the whole volume, relative to codec 0 dB in
				 * steps of 0.5 dB; lidar-dsp's share is the part
				 * below the cap (v[2]) */
				vol_db = (v[0] - 127) / 2.0f;
				in_loop_db = exists(DSP) ? (v[0] - v[2]) / 2.0f : 0;
			} else if (r < 0) {
				dark = 0;		/* no volume service */
				vol_db = in_loop_db = 0;
			}
		}
		if (exists(AEC)) {	/* the speakerphone's echo canceller */
			if (pcm) {
				snd_pcm_close(pcm);
				pcm = NULL;
			}
			if (mine())
				leds_dark(percent);
			usleep(200000);
			continue;
		}
		if (!pcm) {
			pcm = capture_open(dev);
			if (!pcm) {
				if (!said_busy)
					say("%s not available, retrying\n", dev);
				said_busy = 1;
				if (mine())
					leds_dark(percent);
				sleep(1);
				continue;
			}
			said_busy = 0;
		}
		/* a capture stream starts with its first read, and this waits
		 * before reading: start it (after opening, after a recovery) */
		if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED)
			snd_pcm_start(pcm);
		/* nothing plays and the output is closed: no data, no display */
		int w = snd_pcm_wait(pcm, 200);
		if (w <= 0) {
			if (w < 0 && snd_pcm_recover(pcm, w, 1) < 0) {
				snd_pcm_close(pcm);
				pcm = NULL;
			}
			if (mine())
				leds_dark(percent);
			continue;
		}
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, HOP);
		if (n < 0) {
			if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
				snd_pcm_close(pcm);
				pcm = NULL;
				if (mine())
					leds_dark(percent);
			}
			continue;
		}
		/* the newest n frames, mono, into the history */
		memmove(st.hist, st.hist + n, sizeof(float) * (NFFT - n));
		for (int i = 0; i < n; i++)
			st.hist[NFFT - n + i] = (buf[2 * i] + buf[2 * i + 1]) / 65536.0f;
		float rms = 0;
		for (int i = 0; i < NFFT; i++)
			rms += st.hist[i] * st.hist[i];
		float loop_db = 10 * log10f(fmaxf(rms / NFFT, 1e-12f));

		/* silence is judged before the volume, the level at the speaker */
		float rgb[N][3];
		int silent = dark || loop_db - in_loop_db < SILENT_DB;
		if (style_level)
			level(&st, loop_db + vol_db - in_loop_db, vol_db, silent, rgb);
		else
			spectrum(&st, silent, rgb);
		if (mine())
			leds_show(rgb, percent);
	}
	if (mine())
		leds_dark(percent);
	if (pcm)
		snd_pcm_close(pcm);
	return 0;
}
