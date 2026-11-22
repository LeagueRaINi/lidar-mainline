#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Owner of the LED ring after boot. Base state as on stock: solid red while
# the microphones are muted by the privacy button, off otherwise. Other
# programs send short overlays through /run/lidar-ring.fifo:
#   volume <level> <muted 0|1> <max>   arc of white LEDs (dimmed to 15% while
#                                      the speaker is muted), shown for
#                                      HOLD seconds after the last change
#   pair on|ok|off                     Bluetooth pairing mode: a blue arc
#                                      circles until "ok" (two blue
#                                      flashes) or "off"
#   wake <led 1-12>|think|speak|off   the voice assistant: the LED towards
#                                      the speaker and, dimmer, its two
#                                      neighbours in cyan while it listens
#                                      (WAKE_HOLD s at most), pulsing while
#                                      the answer is worked out (THINK_STEPS),
#                                      steady while it speaks (SPEAK_HOLD s
#                                      after the last "speak"), until "off";
#                                      think and speak only follow a wake;
#                                      not while the mics are muted
#   busy <reason> on|off               background work keeps the CPU busy
#                                      (e.g. "busy prepare on" while the
#                                      voice renders its phrases): an orange
#                                      arc circles while any reason is on,
#                                      below every other display
# A change of the privacy state ends any overlay at once; while the mics
# are muted the pairing arc waits under the red ring.
# Brightness follows the ambient light sensor, as on stock: full from a lit
# room down to "ring min_brightness" percent in the dark (a change of it
# applies within seconds; setting "ring auto_brightness off" keeps it at
# full).
# While the ring shows any of this, /run/lidar-ring.busy exists, and
# /run/lidar-ring.level holds the brightness in percent: the visualizer
# (lidar-viz) only draws without the first, at the second.
# Writers should open the FIFO read-write ("exec 5<>/run/lidar-ring.fifo") so
# a write never blocks.
#
# Usage: lidar-ring daemon
FIFO=/run/lidar-ring.fifo
BUSY=/run/lidar-ring.busy
LEVEL=/run/lidar-ring.level
L=/sys/class/leds
N=12
HOLD=2
WAKE_HOLD=12	# listening: speech recognition ends or gives up before this
THINK_STEPS=250	# thinking: pulse steps of PULSE s (Claude has 20 s at most)
SPEAK_HOLD=60
PULSE=0.1
WHITE=96
RED=160
BLUE=160
ORANGE_R=160	# busy: the arc's red and green
ORANGE_G=56
BUSY_SPIN=0.15	# busy: seconds per step, calmer than pairing
WAKE_G=70	# listening: cyan, green and blue of the middle LED
WAKE_B=160
SPIN=0.1	# pairing arc: seconds per step
# Sensor counts at its most sensitive setting: covered reads 0-20, a dimly
# lit room about 550, a flashlight about 9000. Brightness is interpolated on
# a log scale between ALS_DARK (minimum) and ALS_BRIGHT (full).
ALS_DARK=30
ALS_BRIGHT=8000

set_led() { # n r g b
	echo "$2" > "$L/ring$1:red/brightness"
	echo "$3" > "$L/ring$1:green/brightness"
	echo "$4" > "$L/ring$1:blue/brightness"
}

fill() { # r g b
	i=1
	while [ $i -le $N ]; do set_led $i "$1" "$2" "$3"; i=$((i + 1)); done
}

# Ambient light sensor (TSL2540): most sensitive gain and longest
# integration, so a dark room still reads hundreds of counts.
als_setup() {
	als=
	[ "$(lidar-feature get ring auto_brightness 2>/dev/null)" = off ] && return
	for d in /sys/bus/iio/devices/iio:device*; do
		[ "$(cat "$d/name" 2>/dev/null)" = tsl2540 ] && als=$d
	done
	[ -n "$als" ] || return
	tr ' ' '\n' < "$als/in_illuminance_scale_available" | sort -g | head -n1 \
		> "$als/in_illuminance_scale"
	tr -d '[]' < "$als/integration_time_available" | awk '{ print $NF }' \
		> "$als/integration_time"
}

# The ring is ours while BUSY exists: lidar-viz keeps off it. A new claim
# waits two of lidar-viz's frames, so one it is writing ends before this
# draws.
claim() {
	[ -e $BUSY ] && return
	: > $BUSY
	sleep 0.05
}
release() { rm -f $BUSY; }

# min_brightness is read here whenever lidar.conf changed, so a change
# shows without a restart.
level() { # -> P, the brightness in percent, also published for lidar-viz
	P=100
	if [ -n "$als" ]; then
		# re-read only when lidar.conf changed (mtime and inode; a change
		# replaces the file): a lidar-feature fork is not free
		m=$(stat -c '%Y %i' /media/system/etc/lidar.conf 2>/dev/null)
		if [ "$m" != "${dim_at-unread}" ] || [ -z "$dim_min" ]; then
			dim_at=$m
			dim_min=$(lidar-feature get ring min_brightness 2>/dev/null)
			dim_min=${dim_min:-15}
		fi
		P=$(awk -v v="$(cat "$als/in_illuminance_raw" 2>/dev/null)" -v min="$dim_min" \
			-v lo=$ALS_DARK -v hi=$ALS_BRIGHT 'BEGIN {
			if (v == "" || v >= hi) { print 100; exit }
			if (v <= lo) { print min; exit }
			printf "%d\n", min + (100 - min) * (log(v) - log(lo)) / (log(hi) - log(lo))
		}')
	fi
	[ "$P" = "$p_pub" ] || { echo "$P" > $LEVEL; p_pub=$P; }
}

scaled() { # value -> value * P / 100, at least 1 unless 0
	s=$(( $1 * P / 100 ))
	[ "$1" -gt 0 ] && [ $s -lt 1 ] && s=1
	echo $s
}

# The resting state: red while the mics are muted, else dark and the ring
# free for lidar-viz (unless an overlay, pairing or busy arc goes on).
# Claimed first in any case: lidar-viz then sees the hand-back and redraws
# all LEDs.
base() {
	claim
	level
	p_drawn=$P
	if [ "$mic_muted" = 1 ]; then fill $(scaled $RED) 0 0; else fill 0 0 0; fi
	[ "$mic_muted" = 1 ] || [ -n "$pairing$overlay$work" ] || release
}

# The arc covers the level in brightness units: full LEDs, then one partly
# lit LED, so every volume step changes the ring (12 LEDs, 21 button steps).
# While the speaker is muted the same arc shows at 15%: muted, and the level
# it comes back at.
volume() { # level muted max
	[ "$3" -gt 0 ] 2>/dev/null && [ "$1" -ge 0 ] 2>/dev/null || return
	claim
	level
	w=$(scaled $WHITE)
	t=$(( ($1 * N * w + $3 - 1) / $3 ))
	[ "$2" = 1 ] && [ $t = 0 ] && t=1	# muted at level 0: still a mark
	i=1
	while [ $i -le $N ]; do
		v=$t; [ $v -gt $w ] && v=$w
		if [ "$2" = 1 ] && [ $v -gt 0 ]; then
			v=$((v * 15 / 100)); [ $v -lt 1 ] && v=1
		fi
		set_led $i $v $v $v
		t=$((t - w)); [ $t -lt 0 ] && t=0
		i=$((i + 1))
	done
}

# Circling arc: a full LED with a fading tail of two, one step per call;
# blue for pairing, orange while busy. Brightness is read once per turn
# (and when the colour changes), not at every step.
spin() { # blue|orange
	claim
	if [ $pos = 0 ] || [ "$1" != "$spun" ]; then
		spun=$1
		level
		if [ "$1" = orange ]; then
			sr=$(scaled $ORANGE_R) sg=$(scaled $ORANGE_G) sb=0
		else
			sr=0 sg=0 sb=$(scaled $BLUE)
		fi
	fi
	set_led $(( (pos + 9) % N + 1 )) 0 0 0
	set_led $(( (pos + 10) % N + 1 )) $(( sr / 8 )) $(( sg / 8 )) $(( sb / 8 ))
	set_led $(( (pos + 11) % N + 1 )) $(( sr / 3 )) $(( sg / 3 )) $(( sb / 3 ))
	set_led $(( pos + 1 )) $sr $sg $sb
	pos=$(( (pos + 1) % N ))
}

# The voice assistant: the LED towards the speaker, its neighbours at a
# fifth. wake_at picks the LED and reads the brightness once (level forks);
# wake_draw draws at a percentage of that, for the pulse while thinking.
wake_at() { # led
	claim
	level
	wake_led=$1
	wg=$(scaled $WAKE_G)
	wb=$(scaled $WAKE_B)
	wake_draw 100
}

wake_draw() { # percent
	g=$((wg * $1 / 100))
	b=$((wb * $1 / 100))
	i=1
	while [ $i -le $N ]; do
		case $(( (i - wake_led + N) % N )) in
		0) set_led $i 0 $g $b ;;
		1 | $((N - 1))) set_led $i 0 $((g / 5)) $((b / 5)) ;;
		*) set_led $i 0 0 0 ;;
		esac
		i=$((i + 1))
	done
}

flash() { # two blue flashes: paired
	claim
	level
	for i in 1 2; do
		fill 0 0 $(scaled $BLUE); sleep 0.25
		fill 0 0 0; sleep 0.15
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
	pairing=
	pos=0
	work=		# reasons for the busy arc, space-separated
	p_drawn=100
	als_setup
	mic_watch &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	base
	while :; do
		t=$HOLD
		case $overlay in
		wake) t=$WAKE_HOLD ;;
		think) t=$PULSE ;;
		speak) t=$SPEAK_HOLD ;;
		esac
		[ -n "$pairing" ] && [ -z "$overlay" ] && [ "$mic_muted" != 1 ] && t=$SPIN
		[ -z "$pairing" ] && [ -n "$work" ] && [ -z "$overlay" ] && [ "$mic_muted" != 1 ] && t=$BUSY_SPIN
		if read -t $t cmd a b c <&3; then
			case $cmd in
			mic) mic_muted=$a; overlay=; pos=0; base ;;	# privacy state wins at once
			volume) volume "$a" "$b" "$c"; overlay=1 ;;
			wake)
				case $a in
				[1-9] | 1[0-2]) [ "$mic_muted" = 1 ] || { wake_at "$a"; overlay=wake; } ;;
				think) case $overlay in wake | speak) overlay=think; step=0; steps=0 ;; esac ;;
				speak) case $overlay in wake | think | speak) overlay=speak; wake_draw 100 ;; esac ;;
				off) case $overlay in wake | think | speak) overlay=; pos=0; base ;; esac ;;
				esac
				;;
			pair)
				case $a in
				on) pairing=1; pos=0; [ -n "$overlay" ] || { claim; base; } ;;
				ok) pairing=; overlay=; [ "$mic_muted" = 1 ] || flash; base ;;
				*) pairing=; overlay=; base ;;
				esac
				;;
			busy)
				case $a in *[!a-z0-9_-]* | '') a= ;; esac
				if [ -n "$a" ]; then
					work=$(echo "$work" | tr ' ' '\n' | grep -vx -e "$a" -e '' | tr '\n' ' ')
					[ "$b" = on ] && work="$work$a "
					work=${work# }
					[ -n "$work" ] || [ -n "$pairing$overlay" ] || { pos=0; base; }
				fi
				;;
			esac
		elif [ "$overlay" = think ] && [ $steps -lt $THINK_STEPS ]; then
			# breathe between 100 and 30 % in 1.2 s
			step=$(( (step + 1) % 12 ))
			steps=$((steps + 1))
			d=$((step - 6))
			[ $d -lt 0 ] && d=$((-d))
			wake_draw $((30 + d * 70 / 6))
		elif [ -n "$overlay" ]; then
			overlay=
			pos=0
			base
		elif [ -n "$pairing" ] && [ "$mic_muted" != 1 ]; then
			spin blue
		elif [ -n "$work" ] && [ "$mic_muted" != 1 ]; then
			spin orange
		elif [ "$mic_muted" = 1 ]; then
			# follow the room light in steps that do not flicker; the end
			# values are clamped, so reaching them cannot flicker either
			level
			d=$((P - p_drawn)); [ $d -lt 0 ] && d=$((-d))
			if [ $d -ge 10 ] || { [ $d -gt 0 ] &&
			   { [ $P = 100 ] || [ $P = "$dim_min" ]; }; }; then
				base
			fi
		else
			level	# brightness for lidar-viz, every HOLD seconds
		fi
	done
}

case $1 in
daemon) daemon ;;
*)
	sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
