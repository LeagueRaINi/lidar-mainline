#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System volume for the Echo speaker: one level and a mute flag, applied to
# the codec's PCM Playback Volume (0.5 dB steps). Every source of volume
# changes feeds the same state:
#   - a Bluetooth phone's volume (AVRCP absolute volume, through
#     lidar-bluetooth: "bt <0-127>"), which also hears back;
#   - the volume buttons (hold to repeat; a short tone at the new level);
#   - this command.
#
# The level runs from 0 (silent: muted like the mute flag, since the codec's
# lowest step, -63.5 dB, is still audible) to the safety cap set by
# "lidar-feature set volume max_attenuation <dB>".
#
# With the speaker tuning (dsp feature) the volume is applied by lidar-dsp,
# before its EQ and compressor as on stock: lidar-volume writes the level to
# /run/lidar-dsp/volume and, once lidar-dsp reports that its output runs,
# parks the codec at the safety cap. Until then the codec keeps the level
# too, so a missing DSP can only make the speaker quieter, never louder.
#
# The speaker output is held open by a silent stream (volume keepalive), as
# on stock: every start and stop of the codec stream clicks, either through
# the DAC clocks stopping or through the amplifier's mute pin.
#
# Usage: lidar-volume daemon
#        lidar-volume get
#        lidar-volume set <percent> | up | down | mute | unmute | toggle
CODEC=amazonlidar
RUN=/run/lidar-volume
FIFO=$RUN.fifo
RING_FIFO=/run/lidar-ring.fifo	# lidar-ring draws the volume arc
STEP=6		# volume buttons: 3 dB per press
REPEAT_DELAY=0.4	# held button: first repeat after this many seconds,
REPEAT_EVERY=0.15	# then one step every this many seconds
TONE=/usr/share/lidar/volume-tone.wav
DSP=/run/lidar-dsp
ECHO=150	# centiseconds the phone's events after a write count as its echo

a=$(lidar-feature get volume max_attenuation 2>/dev/null)
VMAX=$((127 - 2 * ${a:-20}))

now_cs() { # uptime in centiseconds
	read -r u _ < /proc/uptime
	echo $(( ${u%.*} * 100 + 1${u#*.} - 100 ))
}

# Codec output, and the Bluetooth phone's copy so its slider follows changes
# made here. Equal values are not written again; a change that came from
# the phone does not go back to it. The phone answers a write with its own
# rounding of it (an iPhone to 16 steps): for ECHO centiseconds after a
# write, its events are taken as that echo and ignored. "show" puts the
# volume on the LED ring.
apply() {
	# Level 0 is silence: the codec's lowest step is -63.5 dB, still audible,
	# so it counts as muted (the state keeps the level and the mute flag)
	silent=$mute; [ "$level" = 0 ] && silent=1
	c=$level
	[ "$silent" = 1 ] && c=0
	if [ -e $DSP/active ]; then
		awk -v l="$level" -v m="$VMAX" -v u="$silent" \
			'BEGIN { printf "%.1f %.1f %d\n", (l - 127) / 2, (l - m) / 2, u }' > $DSP/volume.new &&
			mv $DSP/volume.new $DSP/volume
		# a lidar-dsp killed without cleanup leaves ready behind
		[ -e $DSP/ready ] && pidof lidar-dsp >/dev/null && c=$VMAX
	fi
	amixer -q -c $CODEC cset name='PCM Playback Volume' "$c,$c"
	# Without lidar-dsp muting the samples, silence takes the output
	# drivers' mute (DAC to line out and headphone driver, both feed the
	# amplifier); written only when it changes
	d=on
	[ "$silent" = 1 ] && [ "$c" = 0 ] && d=off
	if [ "$d" != "$drivers" ]; then
		amixer -q -c $CODEC cset name='LO DAC Playback Switch' "$d,$d"
		amixer -q -c $CODEC cset name='HP DAC Playback Switch' "$d,$d"
		drivers=$d
	fi
	# the phone has no mute: only the level goes there
	if [ -z "$from_bt" ] && pidof bluealsa >/dev/null; then
		v=$(( (level * 127 + VMAX / 2) / VMAX ))
		for p in $(bluealsa-cli list-pcms 2>/dev/null); do
			case $p in */a2dpsnk/source) ;; *) continue ;; esac
			bluealsa-cli volume "$p" 2>/dev/null | grep -q "L: $v R: $v" && continue
			bluealsa-cli volume "$p" "$v" 2>/dev/null
			bt_echo=$(( $(now_cs) + ECHO ))
		done
	fi
	echo "$level $mute $VMAX" > $RUN.state
	[ "$1" = show ] && show
}

# The ring belongs to lidar-ring (the ring service); its FIFO is opened
# read-write once it exists, so a write never blocks.
show() {
	[ "$ring_on" = on ] || return
	if [ -z "$ring_open" ]; then
		[ -p $RING_FIFO ] || return
		exec 4<> $RING_FIFO
		ring_open=1
	fi
	echo "volume $level $mute $VMAX" >&4
}

clamp() { # value -> 0..VMAX
	if [ "$1" -lt 0 ]; then echo 0
	elif [ "$1" -gt $VMAX ]; then echo $VMAX
	else echo "$1"; fi
}

# Volume buttons: a press steps once, holding repeats until release. The
# feedback tone sounds on the press and, after a hold, once at the final
# level, not on every repeat.
buttons() {
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest "/dev/input/$ev" 2>/dev/null | {
		rp=
		held=
		stop() {
			[ -n "$rp" ] && kill "$rp" 2>/dev/null && wait "$rp" 2>/dev/null
			rp=
			held=
		}
		hold() { # direction
			stop
			(sleep $REPEAT_DELAY
			 while :; do echo "$1" >&3; sleep $REPEAT_EVERY; done) &
			rp=$!
			held=$1
		}
		while read -r line; do
			case $line in
			*KEY_VOLUMEUP*'value 1'*) echo "up tone" >&3; hold up ;;
			*KEY_VOLUMEDOWN*'value 1'*) echo "down tone" >&3; hold down ;;
			*KEY_VOLUMEUP*'value 0'*) [ "$held" = up ] && stop && echo "tone" >&3 ;;
			*KEY_VOLUMEDOWN*'value 0'*) [ "$held" = down ] && stop && echo "tone" >&3 ;;
			esac
		done
		stop
	}
}

# Short feedback tone at the current volume. A tone still playing is not cut
# off (that clicks); the new one is skipped. If the codec output is closed
# (volume keepalive off), 600 ms of silence go first: the amplifier only
# unmutes 500 ms after the DAC starts, and would otherwise chop the tone.
tone() {
	[ "$sound_on" = on ] && [ "$mute" = 0 ] && [ -r $TONE ] || return
	[ -n "$tp" ] && kill -0 "$tp" 2>/dev/null && return
	if grep -qs closed /proc/asound/$CODEC/pcm0p/sub0/status; then
		viz_quiet $TONE 0.6
		{ head -c 115200 /dev/zero; tail -c +45 $TONE; } |
			aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 2>/dev/null &
	else
		viz_quiet $TONE 0
		aplay -q -D lidar_speaker $TONE 2>/dev/null &
	fi
	tp=$!
}

# The sound display keeps off our own sounds: until the time (seconds since
# boot) in /run/lidar-viz.quiet. $1 a 48 kHz 16-bit stereo WAV, $2 seconds
# of silence before it; 0.3 s more for the speaker tuning and buffers.
viz_quiet() { # the later end wins when sounds overlap; replaced, never half written
	b=$(stat -c %s "$1" 2>/dev/null) || return 0
	read -r up _ < /proc/uptime
	old=0; [ -r /run/lidar-viz.quiet ] && read -r old < /run/lidar-viz.quiet
	awk -v u="$up" -v b="$b" -v x="$2" -v o="${old:-0}" \
		'BEGIN { t = u + (b - 44) / 192000 + x + 0.3; printf "%.2f\n", t > o + 0 ? t : o + 0 }' \
		> /run/lidar-viz.quiet.$$ && mv /run/lidar-viz.quiet.$$ /run/lidar-viz.quiet
}

keepalive() { # silence into dmix holds the codec output open
	while :; do
		aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 /dev/zero 2>/dev/null
		sleep 1
	done
}

# Producers write to fd 3, inherited from the daemon: if a daemon dies, its
# leftover producers write into its own FIFO handle, never into a new
# daemon's. On TERM the whole process group goes.
daemon() {
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	ring_on=$(lidar-feature get volume ring 2>/dev/null)
	ring_open=
	sound_on=$(lidar-feature get volume sound 2>/dev/null)
	tp=
	last_tone=
	p=$(lidar-feature get volume initial 2>/dev/null)
	level=$(clamp $(( (${p:-30} * VMAX + 50) / 100 )))
	mute=0
	bt_echo=0
	apply
	buttons &
	[ "$(lidar-feature get volume keepalive 2>/dev/null)" = off ] || keepalive &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	while read -r cmd arg <&3; do
		case $cmd in
		up) level=$(clamp $((level + STEP))); mute=0; apply show ;;
		down) level=$(clamp $((level - STEP))); apply show ;;
		set) level=$(clamp $(( (arg * VMAX + 50) / 100 ))); apply show ;;
		mute) mute=1; apply show ;;
		unmute) mute=0; apply show ;;
		toggle) mute=$((1 - mute)); apply show ;;
		bt)	# the phone's volume, 0-127 (lidar-bluetooth)
			[ "$(now_cs)" -lt "$bt_echo" ] && continue
			case $arg in '' | *[!0-9]*) continue ;; esac
			[ "$arg" -le 127 ] || continue
			level=$(clamp $(( (arg * VMAX + 63) / 127 ))); mute=0
			from_bt=1; apply show; from_bt= ;;
		refresh) apply ;;	# lidar-dsp started or stopped
		tone)	# release: only if a hold moved the level since the press
			[ "$level" != "$last_tone" ] && tone && last_tone=$level ;;
		esac
		# a press: tone right away at the new level
		if [ "$arg" = tone ]; then tone; last_tone=$level; fi
	done
}

send() {
	[ -p $FIFO ] || { echo "lidar-volume daemon is not running (lidar-feature enable volume)" >&2; exit 1; }
	echo "$*" > $FIFO
}

case $1 in
daemon) daemon ;;
get)
	[ -f $RUN.state ] || { echo "lidar-volume daemon is not running" >&2; exit 1; }
	read -r level mute vmax < $RUN.state
	db=$(awk -v l="$level" 'BEGIN { printf "%.1f", (l - 127) / 2 }')
	echo "volume $(( (level * 100 + vmax / 2) / vmax ))% (codec $level of $vmax, $db dB)$([ "$mute" = 1 ] && echo ", muted")"
	;;
set)
	case $2 in '' | *[!0-9]* | 0?*) echo "usage: lidar-volume set <0-100>" >&2; exit 2 ;; esac
	[ "$2" -le 100 ] || { echo "usage: lidar-volume set <0-100>" >&2; exit 2; }
	send set "$2"
	;;
up | down | mute | unmute | toggle) send "$1" ;;
*)
	sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
