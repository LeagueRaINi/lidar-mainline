#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Owner of the LED ring after boot. Base state as on stock: solid red while
# the microphones are muted by the privacy button, off otherwise. Other
# programs send short overlays through /run/lidar-ring.fifo:
#   volume <level> <muted 0|1> <max>   arc of white LEDs (one dim red LED
#                                      when the speaker is muted), shown for
#                                      HOLD seconds after the last change
# A change of the privacy state ends any overlay at once.
# Writers should open the FIFO read-write ("exec 5<>/run/lidar-ring.fifo") so
# a write never blocks.
#
# Usage: lidar-ring daemon
FIFO=/run/lidar-ring.fifo
L=/sys/class/leds
N=12
HOLD=2
WHITE=96
RED=160

set_led() { # n r g b
	echo "$2" > "$L/ring$1:red/brightness"
	echo "$3" > "$L/ring$1:green/brightness"
	echo "$4" > "$L/ring$1:blue/brightness"
}

fill() { # r g b
	i=1
	while [ $i -le $N ]; do set_led $i "$1" "$2" "$3"; i=$((i + 1)); done
}

base() {
	if [ "$mic_muted" = 1 ]; then fill $RED 0 0; else fill 0 0 0; fi
}

# The arc covers the level in brightness units: full LEDs, then one partly
# lit LED, so every volume step changes the ring (12 LEDs, 21 button steps).
volume() { # level muted max
	[ "$3" -gt 0 ] 2>/dev/null && [ "$1" -ge 0 ] 2>/dev/null || return
	t=0
	[ "$2" = 1 ] || t=$(( ($1 * N * WHITE + $3 - 1) / $3 ))
	i=1
	while [ $i -le $N ]; do
		if [ "$2" = 1 ] && [ $i = 1 ]; then set_led $i 24 0 0
		elif [ $t -ge $WHITE ]; then set_led $i $WHITE $WHITE $WHITE
		else set_led $i $t $t $t
		fi
		t=$((t - WHITE)); [ $t -lt 0 ] && t=0
		i=$((i + 1))
	done
}

# Privacy latch (SW_MUTE_DEVICE on gpio-keys) into the FIFO
mic_watch() {
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest --query "/dev/input/$ev" EV_SW SW_MUTE_DEVICE
	[ $? = 10 ] && echo "mic 1" >&3 || echo "mic 0" >&3
	evtest "/dev/input/$ev" 2>/dev/null | while read -r line; do
		case $line in
		*SW_MUTE_DEVICE*'value 1'*) echo "mic 1" >&3 ;;
		*SW_MUTE_DEVICE*'value 0'*) echo "mic 0" >&3 ;;
		esac
	done
}

daemon() {
	[ -e "$L/ring1:red/brightness" ] || exit 0
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	# The boot animation owns the ring until it has finished its fade-out.
	while [ ! -e /run/boot-done ] || pidof lidar-bootanim >/dev/null; do
		sleep 0.2
	done
	# Overlays sent in the meantime are stale: show none of them.
	while read -t 0.05 _ <&3; do :; done
	mic_muted=0
	overlay=
	mic_watch &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	base
	while :; do
		if read -t $HOLD cmd a b c <&3; then
			case $cmd in
			mic) mic_muted=$a; overlay=; base ;;	# privacy state wins at once
			volume) volume "$a" "$b" "$c"; overlay=1 ;;
			esac
		elif [ -n "$overlay" ]; then
			overlay=
			base
		fi
	done
}

case $1 in
daemon) daemon ;;
*)
	sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
