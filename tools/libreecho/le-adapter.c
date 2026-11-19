// SPDX-License-Identifier: GPL-2.0-only
/*
 * le-adapter: lidar side of the LibreEcho add-on. LibreEcho's userspace runs
 * in a chroot (see lidar-libreecho.sh) and expects MT8163 interfaces; this
 * program, outside the chroot, provides two of them:
 *
 *  - LED ring: LibreEcho's ledd writes IS31FL3236 frames ("RRGGBB" x 12, hex)
 *    to <i2c-dir>/frame. That is a plain file in a directory bind-mounted
 *    over the chroot's /sys/bus/i2c/devices; every completed write is applied
 *    to /sys/class/leds/ringN:{red,green,blue}.
 *  - Microphones: LibreEcho's micd reads raw 16 kHz, 9-channel S24_3LE audio
 *    (16 valid bits, left-justified; lanes 0-5 the ring mics, lane 6 the
 *    centre mic, 7-8 unused) from its capture program. This captures
 *    hw:amazonlidar,1 (8 x S32_LE) at 16 kHz and writes that format to a
 *    FIFO, which the capture program inside the chroot simply reads.
 *
 * Usage: le-adapter <i2c-dir> <mic-fifo>
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define RING		12
#define MIC_PCM		"hw:amazonlidar,1"
#define MIC_RATE	16000
#define MIC_IN_CH	8
#define MIC_OUT_CH	9
#define MIC_PERIOD	640		/* frames, 40 ms, as micd asks tinycap */

/*
 * Capture channel -> LibreEcho lane. Lanes 0-5 sit at 150 + 60n degrees on
 * the ring and lane 6 is the centre; our channels 0-5 are the ring mics in
 * board order and channel 6 the centre mic. The ring orientation relative
 * to LibreEcho's is not verified yet (it only rotates beam directions).
 */
static const int lane_of_channel[MIC_IN_CH] = { 0, 1, 2, 3, 4, 5, 6, -1 };

static void die(const char *what)
{
	perror(what);
	exit(1);
}

/* ---------------------------------------------------------------- LEDs */

/* LE_LED_SCALE (percent, default 100) dims everything LibreEcho draws */
static unsigned int led_scale = 100;

static void apply_frame(const char *text)
{
	static int last[RING * 3];
	static int have_last;
	char path[64];

	for (int i = 0; i < RING * 3; i++) {
		static const char *const col[] = { "red", "green", "blue" };
		unsigned int v;
		FILE *f;

		if (sscanf(text + 2 * i, "%2x", &v) != 1)
			return;	/* short or garbled frame: ignore it */
		v = v * led_scale / 100;
		if (have_last && last[i] == (int)v)
			continue;
		last[i] = v;
		snprintf(path, sizeof(path), "/sys/class/leds/ring%d:%s/brightness",
			 i / 3 + 1, col[i % 3]);
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "%u\n", v);
			fclose(f);
		}
	}
	have_last = 1;
}

static void *led_loop(void *arg)
{
	const char *dir = arg;
	char frame_path[256], buf[256];
	int in = inotify_init1(IN_CLOEXEC);

	snprintf(frame_path, sizeof(frame_path), "%s/frame", dir);
	if (in < 0 || inotify_add_watch(in, frame_path, IN_CLOSE_WRITE) < 0)
		die("le-adapter: inotify");
	for (;;) {
		char ev[sizeof(struct inotify_event) + 256];
		int fd;
		ssize_t n;

		if (read(in, ev, sizeof(ev)) <= 0 && errno != EINTR)
			die("le-adapter: inotify read");
		fd = open(frame_path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n >= RING * 6) {
			buf[n] = 0;
			apply_frame(buf);
		}
	}
	return NULL;
}

/* ---------------------------------------------------------- microphones */

static snd_pcm_t *mic_open(void)
{
	snd_pcm_t *pcm;
	int err;

	if ((err = snd_pcm_open(&pcm, MIC_PCM, SND_PCM_STREAM_CAPTURE, 0)) < 0 ||
	    (err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE,
				      SND_PCM_ACCESS_RW_INTERLEAVED, MIC_IN_CH,
				      MIC_RATE, 0, 4LL * MIC_PERIOD * 1000000 / MIC_RATE)) < 0 ||
	    /* set_params leaves the start threshold at the whole buffer, more
	       than one read asks for: start the capture here */
	    (err = snd_pcm_start(pcm)) < 0) {
		fprintf(stderr, "le-adapter: %s: %s\n", MIC_PCM, snd_strerror(err));
		return NULL;
	}
	return pcm;
}

/*
 * One reader at a time: opening the FIFO waits for micd's capture program;
 * the capture runs while it reads and stops when it goes away.
 */
static void *mic_loop(void *arg)
{
	const char *fifo = arg;
	static int32_t in[MIC_PERIOD * MIC_IN_CH];
	static unsigned char out[MIC_PERIOD * MIC_OUT_CH * 3];

	for (;;) {
		int fd = open(fifo, O_WRONLY | O_CLOEXEC);	/* blocks for a reader */
		snd_pcm_t *pcm;

		if (fd < 0)
			die("le-adapter: mic fifo");
		pcm = mic_open();
		while (pcm) {
			snd_pcm_sframes_t n = snd_pcm_readi(pcm, in, MIC_PERIOD);

			if (n < 0) {
				/* recover leaves it prepared: start it again, as
				   in mic_open */
				int err = snd_pcm_recover(pcm, n, 1);

				if (err >= 0)
					err = snd_pcm_start(pcm);
				if (err < 0) {
					fprintf(stderr, "le-adapter: capture: %s\n", snd_strerror(err));
					break;
				}
				continue;
			}
			memset(out, 0, (size_t)n * MIC_OUT_CH * 3);
			for (snd_pcm_sframes_t f = 0; f < n; f++)
				for (int c = 0; c < MIC_IN_CH; c++) {
					int lane = lane_of_channel[c];
					/* top 16 bits, left-justified in 24 */
					int32_t s = (in[f * MIC_IN_CH + c] >> 16) << 8;
					unsigned char *o;

					if (lane < 0)
						continue;
					o = out + (f * MIC_OUT_CH + lane) * 3;
					o[0] = s & 0xff;
					o[1] = (s >> 8) & 0xff;
					o[2] = (s >> 16) & 0xff;
				}
			if (write(fd, out, (size_t)n * MIC_OUT_CH * 3) < 0) {
				if (errno != EPIPE)
					perror("le-adapter: mic write");
				break;	/* EPIPE: the reader went away */
			}
		}
		if (pcm)
			snd_pcm_close(pcm);
		close(fd);
		sleep(1);
	}
	return NULL;
}

/* --------------------------------------------------------- mixer controls */

/*
 * LibreEcho's audio engine sets controls of its MT8163 kernel around every
 * output start (amplifier power and mute, DAC mux, channel config, codec EQ
 * profile) and refuses to play if one is missing. lidar's machine driver
 * powers and mutes its amplifier with the stream and keeps the speaker EQ in
 * the DAC, so these are placeholders: user-defined enumerated controls on
 * the card. They stay until the card goes; one left with another layout is
 * replaced (the kernel only removes user-defined controls).
 */
struct compat_control {
	const char *name;
	unsigned int items;
	const char *const *names;
};

static const char *const off_on[] = { "Off", "On" };
static const char *const profiles[] = { "Radar", "Flat" };
static const char *const channels[] = { "Stereo", "Mono" };

static const struct compat_control compat_controls[] = {
	{ "MFP Gpio Mute", 2, off_on },
	{ "Ext_Speaker_Amp_Switch", 2, off_on },
	{ "Audio_DacMux_Setting", 2, off_on },
	{ "Board Channel Config", 2, channels },
	{ "Right Channel Only", 2, off_on },
	{ "Speaker Codec Profile", 2, profiles },
};

static void add_compat_controls(void)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_t *ctl;

	if (snd_ctl_open(&ctl, "hw:amazonlidar", 0) < 0) {
		fprintf(stderr, "le-adapter: no control device\n");
		return;
	}
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	for (unsigned int i = 0; i < sizeof(compat_controls) / sizeof(compat_controls[0]); i++) {
		const struct compat_control *c = &compat_controls[i];

		snd_ctl_elem_id_clear(id);
		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, c->name);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) == 0) {
			if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_ENUMERATED &&
			    snd_ctl_elem_info_get_items(info) == c->items)
				continue;
			snd_ctl_elem_remove(ctl, id);
		}
		if (snd_ctl_elem_add_enumerated(ctl, id, 1, c->items, c->names) < 0)
			fprintf(stderr, "le-adapter: cannot add %s\n", c->name);
	}
	snd_ctl_close(ctl);
}

int main(int argc, char **argv)
{
	pthread_t led, mic;

	if (argc != 3) {
		fprintf(stderr, "usage: le-adapter <i2c-dir> <mic-fifo>\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	if (getenv("LE_LED_SCALE")) {
		unsigned long s = strtoul(getenv("LE_LED_SCALE"), NULL, 10);

		led_scale = s > 100 ? 100 : s;
	}
	if (mkfifo(argv[2], 0600) < 0 && errno != EEXIST)
		die("le-adapter: mkfifo");
	add_compat_controls();
	if (pthread_create(&led, NULL, led_loop, argv[1]) ||
	    pthread_create(&mic, NULL, mic_loop, argv[2]))
		die("le-adapter: thread");
	pthread_join(led, NULL);
	return 0;
}
