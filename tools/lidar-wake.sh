#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Listens for the wake word: lidar-voice (the speaker's voice out of the
# seven microphones) into lidar-wakeword, restarted if either dies. At each
# detection:
#   - the LED ring shows where the voice came from ("wake <led>" to
#     lidar-ring, the talker direction "doa" from /run/lidar-voice.json)
#   - a short, quiet two-note chime plays at the system volume (not while
#     the speaker is muted); the sound display keeps off it
#   - /run/lidar-wake.events gets a line "wake <seconds since boot> <angle>"
#     (the last 20 are kept), /run/lidar-wake.json holds
#     {"last": <seconds since boot>, "angle": <degrees>, "count": <n>}
#   - /run/lidar-wake.listening exists for 5 s: the time to say something
#     (lidar-stt, with the assistant service on, turns what is said then into
#     a line on /run/lidar-assistant.fifo; it reads its copy of the audio from
#     /run/lidar-stt.pcm, which lidar-tee fills without waiting for it)
#   - lidar-tts stops talking ("stop" on /run/lidar-tts.fifo)
# While the microphones are muted by the privacy button they deliver
# silence, so nothing is detected.
# Settings: voice sensitivity (low, medium, high: the detector's cutoff).
#
# Usage: lidar-wake daemon   (run by init)
#        lidar-wake status
VOICE_STATE=/run/lidar-voice.json
RING=/run/lidar-ring.fifo
EVENTS=/run/lidar-wake.events
STATE=/run/lidar-wake.json
LISTENING=/run/lidar-wake.listening
TTS=/run/lidar-tts.fifo
PCM=/run/lidar-stt.pcm
CHIME=/run/lidar-wake.wav
CODEC=amazonlidar
KEEP=20		# lines in $EVENTS
LISTEN=5	# seconds $LISTENING stays

# The chime: E5 then A5, 90 ms apart, 0.25 s in all, each a sine with a
# little second harmonic, 5 ms attack and an exponential decay, peak about
# -14 dBFS. 48 kHz 16-bit stereo WAV, written once per boot. awk prints the
# bytes as octal escapes for printf: busybox awk cannot write a NUL byte.
chime_make() {
	[ -s $CHIME ] && return
	printf "$(awk 'function le(v, n,    s) {
			for (; n > 0; n--) { s = s sprintf("\\%03o", v % 256); v = int(v / 256) }
			return s
		}
		function note(t, f,    e) {
			if (t < 0) return 0
			e = (t < 0.005 ? t / 0.005 : 1) * exp(-t / 0.06)
			return e * (sin(6.2831853 * f * t) + 0.25 * sin(12.5663706 * f * t))
		}
		BEGIN {
			fs = 48000; n = fs / 4
			printf "RIFF%sWAVEfmt %s%s%s%s%s%s%sdata%s", le(36 + n * 4, 4), le(16, 4),
				le(1, 2), le(2, 2), le(fs, 4), le(fs * 4, 4), le(4, 2), le(16, 2), le(n * 4, 4)
			for (i = 0; i < n; i++) {
				t = i / fs
				v = 5200 * (note(t, 659.26) + note(t - 0.09, 880))
				if (i > n - 960) v *= (n - i) / 960	# 20 ms fade-out
				v = int(v); if (v < 0) v += 65536
				s = le(v, 2)
				printf "%s%s", s, s
			}
		}')" > $CHIME.$$ && mv $CHIME.$$ $CHIME
}

# A feedback sound as the others (lidar-volume, lidar-bluetooth): none while
# the speaker is muted. If the codec output is closed (volume keepalive
# off), 600 ms of silence go first: the amplifier only unmutes 500 ms after
# the DAC starts, and would otherwise chop the sound.
chime() {
	muted=0
	[ -r /run/lidar-volume.state ] && read -r _ muted _ < /run/lidar-volume.state
	[ "$muted" = 1 ] && return 0
	[ -r $CHIME ] || return 0
	# viz_until after aplay: it can take a moment to start, and the sound
	# then ends later than its length says
	if grep -qs closed /proc/asound/$CODEC/pcm0p/sub0/status; then
		viz_quiet $CHIME 0.6
		{ head -c 115200 /dev/zero; tail -c +45 $CHIME; } |
			aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 2>/dev/null
	else
		viz_quiet $CHIME 0
		aplay -q -D lidar_speaker $CHIME 2>/dev/null
	fi
	viz_until 0.6
}

# The sound display keeps off our own sounds: until the time (seconds since
# boot) in /run/lidar-viz.quiet. The later end wins when sounds overlap; the
# file is replaced, never half written.
viz_until() { # seconds from now
	read -r up _ < /proc/uptime
	old=0; [ -r /run/lidar-viz.quiet ] && read -r old < /run/lidar-viz.quiet
	awk -v u="$up" -v s="$1" -v o="${old:-0}" 'BEGIN { t = u + s; printf "%.2f\n", (t > o + 0 ? t : o + 0) }' \
		> /run/lidar-viz.quiet.$$ && mv /run/lidar-viz.quiet.$$ /run/lidar-viz.quiet
}

# $1 a 48 kHz 16-bit stereo WAV about to play, $2 seconds of silence before
# it; 0.3 s more for the speaker tuning and buffers
viz_quiet() {
	b=$(stat -c %s "$1" 2>/dev/null) || return 0
	viz_until "$(awk -v b="$b" -v x="$2" 'BEGIN { print (b - 44) / 192000 + x + 0.3 }')"
}

field() { # file key -> the number after "key": in a one-line JSON file
	[ -r "$1" ] || return 0
	sed -n "s/.*\"$2\": *\\(-\\{0,1\\}[0-9][0-9.]*\\).*/\\1/p" "$1" | head -n1
}

# One detection. $1: its time in seconds since boot, as lidar-wakeword
# printed it. The count comes from the state file, so it survives a restart
# of the pipeline.
on_wake() {
	t=$1
	case $t in
	'' | *[!0-9.]*) read -r t _ < /proc/uptime ;;
	esac
	# first: lidar-stt starts listening when this file changes
	echo "$t" > $LISTENING
	# the Echo stops talking when addressed: lidar-tts drops what it was
	# going to say. Read-write: never waits, whether lidar-tts runs or not
	[ -p $TTS ] && echo stop 1<> $TTS
	# the talker direction (SRP-PHAT, 10 degree steps); the beam's angle
	# only if lidar-voice has heard no speech yet
	angle=$(field $VOICE_STATE doa)
	[ -n "$angle" ] || angle=$(field $VOICE_STATE angle)
	case $angle in
	'' | *[!0-9]*) angle= ;;
	esac
	# Angle (0 at mic 0, clockwise) to the nearest LED: round((angle - 210) / 30) mod 12 + 1
	if [ -n "$angle" ] && [ -p $RING ]; then
		echo "wake $(( ((angle + 165) / 30) % 12 + 1 ))" 1<> $RING
	fi
	chime < /dev/null > /dev/null 2>&1 &
	n=$(field $STATE count)
	n=$(( ${n:-0} + 1 ))
	echo "wake $t ${angle:--}" >> $EVENTS
	tail -n $KEEP $EVENTS > $EVENTS.new && mv $EVENTS.new $EVENTS
	echo "{\"last\": $t, \"angle\": ${angle:-null}, \"count\": $n}" > $STATE.new &&
		mv $STATE.new $STATE
	# A later detection takes the file over: only the last one removes it.
	(
		sleep $LISTEN
		read -r l < $LISTENING && [ "$l" = "$t" ] && rm -f $LISTENING
	) < /dev/null > /dev/null 2>&1 &
}

daemon() {
	chime_make
	rm -f $LISTENING
	# the detector's cutoff: the model's own (0.97) unless asked otherwise
	case $(lidar-feature get voice sensitivity 2>/dev/null) in
	low) cutoff='-t 0.99' ;;
	high) cutoff='-t 0.9' ;;
	*) cutoff= ;;
	esac
	# The pipeline runs in the background: a shell waiting for a foreground
	# command would run the trap only once that has ended.
	trap 'trap "" TERM; kill 0; exit' INT TERM
	# Speech recognition beside the wake word, in a loop of its own: its
	# model takes half a minute to load and outlives pipeline restarts.
	stt=
	if [ -x /usr/bin/lidar-stt ] && lidar-feature enabled assistant; then
		stt=1
		[ -p $PCM ] || { rm -f $PCM; mkfifo -m 600 $PCM; }
		(
			while :; do
				lidar-stt <> $PCM > /dev/null 2>&1
				echo "lidar-wake: lidar-stt ended, starting again in 10 s" > /dev/kmsg
				sleep 10
			done
		) < /dev/null &
	fi
	pause=2
	while :; do
		read -r t0 _ < /proc/uptime
		# $cutoff unquoted: two words or none. Only the pipeline is waited
		# for: a bare wait would also wait for the lidar-stt loop.
		if [ -n "$stt" ]; then
			lidar-voice 2>/dev/null | lidar-tee $PCM | lidar-wakeword $cutoff 2>/dev/null
		else
			lidar-voice 2>/dev/null | lidar-wakeword $cutoff 2>/dev/null
		fi | while read -r w _ _ t; do
				[ "$w" = wake ] && on_wake "$t"
			done &
		wait $!
		read -r t1 _ < /proc/uptime
		ran=$(( ${t1%.*} - ${t0%.*} ))
		# a pipeline that ran a while starts again after 2 s; one that keeps
		# failing at once backs off up to a minute and is logged once
		if [ $ran -ge 10 ]; then
			pause=2
			echo "lidar-wake: lidar-voice or lidar-wakeword ended, starting again" > /dev/kmsg
		elif [ $pause -eq 2 ]; then
			echo "lidar-wake: lidar-voice or lidar-wakeword fails at start, retrying with growing pauses" > /dev/kmsg
		fi
		sleep $pause &
		wait $!
		[ $ran -ge 10 ] || { pause=$((pause * 2)); [ $pause -le 60 ] || pause=60; }
	done
}

status() {
	if pgrep -f 'lidar-wake daemon' > /dev/null; then
		echo "running"
	else
		echo "not running"
	fi
	last=$(field $STATE last)
	if [ -z "$last" ]; then
		echo "no detection since boot"
		return
	fi
	read -r up _ < /proc/uptime
	angle=$(field $STATE angle)
	awk -v u="$up" -v l="$last" -v a="${angle:-unknown}" -v n="$(field $STATE count)" 'BEGIN {
		printf "last detection %d s ago, from %s degrees; %d since boot\n", u - l, a, n }'
}

case $1 in
daemon) daemon ;;
status) status ;;
*)
	sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
