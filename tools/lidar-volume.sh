#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System volume for the Echo speaker: one level and a mute flag, applied to
# the codec's PCM Playback Volume (0.5 dB steps). Every source of volume
# changes feeds the same state:
#   - the USB host's volume slider and mute (UAC2 gadget, lidar-usbaudio),
#     which also hears back about changes made here;
#   - the volume buttons;
#   - this command.
# Other sources (e.g. Bluetooth absolute volume) can send the same commands.
#
# The level runs from 0 (codec -63.5 dB, practically silent) to the safety
# cap set by "lidar-feature set volume max_attenuation <dB>".
#
# Usage: lidar-volume daemon
#        lidar-volume get
#        lidar-volume set <percent> | up | down | mute | unmute | toggle | sync
CODEC=amazonlidar
GADGET=UAC2Gadget
RUN=/run/lidar-volume
FIFO=$RUN.fifo
STEP=6		# volume buttons: 3 dB per press

a=$(lidar-feature get volume max_attenuation 2>/dev/null)
VMAX=$((127 - 2 * ${a:-20}))

ctl_get() { # card name -> first value
	amixer -c "$1" cget name="$2" 2>/dev/null | sed -n 's/^  : values=\([^,]*\).*/\1/p'
}

# Codec output, and the gadget's copy so the host slider follows changes
# made here. Equal values are not written again: that would only echo back
# as another host event.
apply() {
	c=$level
	[ "$mute" = 1 ] && c=0
	amixer -q -c $CODEC cset name='PCM Playback Volume' "$c,$c"
	if [ -e /proc/asound/$GADGET ]; then
		[ "$(ctl_get $GADGET 'PCM Capture Volume')" = "$level" ] ||
			amixer -q -c $GADGET cset name='PCM Capture Volume' "$level"
		s=on; [ "$mute" = 1 ] && s=off
		[ "$(ctl_get $GADGET 'PCM Capture Switch')" = "$s" ] ||
			amixer -q -c $GADGET cset name='PCM Capture Switch' "$s"
	fi
	echo "$level $mute $VMAX" > $RUN.state
}

clamp() { # value -> 0..VMAX
	if [ "$1" -lt 0 ]; then echo 0
	elif [ "$1" -gt $VMAX ]; then echo $VMAX
	else echo "$1"; fi
}

# The host changed its slider or mute: take its values.
from_host() {
	v=$(ctl_get $GADGET 'PCM Capture Volume')
	s=$(ctl_get $GADGET 'PCM Capture Switch')
	[ -n "$v" ] && [ -n "$s" ] || return
	level=$(clamp "$v")
	mute=0; [ "$s" = off ] && mute=1
	apply
}

buttons() { # feed volume-key presses and repeats from gpio-keys
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest "/dev/input/$ev" 2>/dev/null | while read -r line; do
		case $line in
		*KEY_VOLUMEUP*'value 1'* | *KEY_VOLUMEUP*'value 2'*) echo up >&3 ;;
		*KEY_VOLUMEDOWN*'value 1'* | *KEY_VOLUMEDOWN*'value 2'*) echo down >&3 ;;
		esac
	done
}

host_watch() { # the gadget's card comes and goes with lidar-usbaudio
	while :; do
		if [ -e /proc/asound/$GADGET ]; then
			echo sync >&3	# start from our level, not the gadget default
			alsactl monitor hw:$GADGET 2>/dev/null | while read -r line; do
				case $line in *'PCM Capture'*) echo host >&3 ;; esac
			done
		fi
		sleep 1
	done
}

# Producers write to fd 3, inherited from the daemon: if a daemon dies, its
# leftover producers write into its own FIFO handle, never into a new
# daemon's. On TERM the whole process group goes.
daemon() {
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	p=$(lidar-feature get volume initial 2>/dev/null)
	level=$(clamp $(( (${p:-30} * VMAX + 50) / 100 )))
	mute=0
	apply
	buttons &
	host_watch &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	while read -r cmd arg <&3; do
		case $cmd in
		up) level=$(clamp $((level + STEP))); mute=0; apply ;;
		down) level=$(clamp $((level - STEP))); apply ;;
		set) level=$(clamp $(( (arg * VMAX + 50) / 100 ))); apply ;;
		mute) mute=1; apply ;;
		unmute) mute=0; apply ;;
		toggle) mute=$((1 - mute)); apply ;;
		host) from_host ;;
		sync) apply ;;
		esac
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
up | down | mute | unmute | toggle | sync) send "$1" ;;
*)
	sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
