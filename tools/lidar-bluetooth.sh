#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bluetooth speaker: phones pair with the Echo and play on its speaker
# (A2DP sink, SBC and AAC). bluetoothd runs the stack, bluealsa the audio
# transport, bluealsa-aplay plays every connected phone into lidar_speaker
# (system volume, speaker tuning, mixed with other sounds). The phone's
# volume (AVRCP absolute volume) is the system volume: changes go to
# lidar-volume, and lidar-volume sends its changes back to the phone
# (without the volume service, bluealsa scales the phone's audio by it).
#
# Pairing: holding the action button 3 s (or "lidar-bluetooth pair") makes
# the Echo visible and pairable for "bluetooth pair_seconds", with a blue
# arc circling on the LED ring and a soft double blip every 3 s
# ("bluetooth sound"); playback pauses. Pairing needs no PIN
# (NoInputNoOutput). The first phone that pairs (also again, after it
# forgot the Echo) ends the mode with two blue flashes and a rising chime,
# and is trusted. Holding the button 3 s again (or "lidar-bluetooth pair
# stop") ends the mode early; that, and nobody pairing, ends it with a
# falling tone.
# Trusted phones connect at any time (with the chime); outside pairing
# mode a phone that is not trusted is disconnected and removed as soon as
# it opens an audio stream. The Echo itself never connects to a phone, not
# at boot and not after the link was lost (out of range). Pairing keys live
# in /var/lib/bluetooth on the booted slot.
#
# Usage: lidar-bluetooth bluetoothd|bluealsa|aplay|daemon   (run by init)
#        lidar-bluetooth pair [stop]      start or end pairing mode
#        lidar-bluetooth status
#        lidar-bluetooth devices          paired phones: address, connected
#                                         (yes/no) and name, tab-separated
#        lidar-bluetooth forget <address> remove a paired phone
#        lidar-bluetooth media play|pause|next|previous
#                                         every connected phone's player
#                                         (AVRCP) and the web player
RUN=/run/lidar-bluetooth
FIFO=$RUN.fifo
PAIRING=$RUN.pairing
STOP=$RUN.stop
RING_FIFO=/run/lidar-ring.fifo
VOLUME_FIFO=/run/lidar-volume.fifo
HOLD=3	# seconds the action button is held for pairing mode
SOUNDS=/usr/share/lidar
CODEC=amazonlidar

log() { echo "lidar-bluetooth: $*" > /dev/kmsg; }

# A daemon's output into the kernel log, line by line, as "<name>: "
to_kmsg() {
	while IFS= read -r l; do
		[ -z "$l" ] || printf '%s: %s\n' "$1" "$l" > /dev/kmsg
	done
}

# A feedback sound at the system volume: none while the speaker is muted
# or with "bluetooth sound off". If the codec output is closed (volume
# keepalive off), 600 ms of silence go first: the amplifier only unmutes
# 500 ms after the DAC starts, and would otherwise chop the sound.
sound() { # pairing | paired | timeout
	[ "$sound_on" = on ] || return 0
	muted=0
	[ -r /run/lidar-volume.state ] && read -r _ muted _ < /run/lidar-volume.state
	[ "$muted" = 1 ] && return 0
	if grep -qs closed /proc/asound/$CODEC/pcm0p/sub0/status; then
		viz_quiet "$SOUNDS/bt-$1.wav" 0.6
		{ head -c 115200 /dev/zero; tail -c +45 "$SOUNDS/bt-$1.wav"; } |
			aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 2>/dev/null
	else
		viz_quiet "$SOUNDS/bt-$1.wav" 0
		aplay -q -D lidar_speaker "$SOUNDS/bt-$1.wav" 2>/dev/null
	fi
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

wait_for() { # seconds command...: true once the command succeeds
	n=$1; shift
	for i in $(seq 1 "$n"); do
		"$@" >/dev/null 2>&1 && return 0
		sleep 1
	done
	return 1
}

# init found no Wi-Fi firmware to stage: no controller will come, so wait
# quietly instead of logging every minute
no_firmware() { [ -e /run/lidar-no-radio-firmware ] && { sleep 3600; exit 1; }; }

uptime_s() { cut -d. -f1 /proc/uptime; }

# bluetoothctl waits for bluetoothd without end: every call has a timeout
btctl() { timeout 2 bluetoothctl "$@" 2>/dev/null; }
bluetoothd_up() { btctl show; }
bluealsa_up() { bluealsa-cli status; }

# FIFO writes that never block: lidar-ring and lidar-volume may be absent
to_fifo() { # fifo line
	[ -p "$1" ] && echo "$2" 1<> "$1"
}

# address per line
devices_of() { btctl devices "$1" | awk '$1 == "Device" { print $2 }'; }
has() { btctl info "$1" | grep -q "^[[:space:]]*$2: yes"; }	# address property
name_of() { btctl info "$1" | sed -n 's/^[[:space:]]*Name: //p'; }

# Pairing mode, in the background of the daemon. bluetoothd ends
# discoverable and pairable itself after the same time (main.conf), so the
# mode ends at that deadline. Done when a phone is paired that was not
# paired, or connects paired that was not connected: a phone that forgot
# the Echo pairs again under an address the Echo still has as paired.
pairing() {
	secs=$(lidar-feature get bluetooth pair_seconds)
	sound_on=$(lidar-feature get bluetooth sound)
	was_paired=" $(devices_of Paired | tr '\n' ' ') "
	was_connected=" $(devices_of Connected | tr '\n' ' ') "
	to_fifo $RING_FIFO "pair on"
	pause_all
	btctl pairable on >/dev/null
	btctl discoverable on >/dev/null
	log "pairing mode for $secs s"
	while :; do sound pairing; sleep 2.4; done &	# 0.6 s sound: one every 3 s
	sp=$!
	end=$(( $(uptime_s) + secs ))
	new=
	while [ "$(uptime_s)" -lt $end ]; do
		sleep 1
		[ -e $STOP ] && break
		for m in $(devices_of Paired); do
			case $was_paired in *" $m "*) ;; *) new=$m ;; esac
		done
		for m in $(devices_of Connected); do
			case $was_connected in *" $m "*) ;; *) has "$m" Paired && new=$m ;; esac
		done
		[ -n "$new" ] && break
	done
	kill $sp
	wait $sp 2>/dev/null
	btctl discoverable off >/dev/null
	btctl pairable off >/dev/null
	if [ -n "$new" ]; then
		btctl trust "$new" >/dev/null
		log "paired: $(name_of "$new")"
		to_fifo $RING_FIFO "pair ok"
		sound paired
	else
		if [ -e $STOP ]; then
			log "pairing mode ended"
		else
			log "pairing mode ended, nobody paired"
		fi
		to_fifo $RING_FIFO "pair off"
		sound timeout
	fi
	rm -f $PAIRING $STOP
}

# Action button (KEY_HELP on gpio-keys): held HOLD seconds -> "toggle"
button_watch() {
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest "/dev/input/$ev" 2>/dev/null | {
		tp=
		while read -r line; do
			case $line in
			*KEY_HELP*'value 1'*)
				[ -n "$tp" ] && kill "$tp" 2>/dev/null
				(sleep $HOLD; echo toggle >&3) &
				tp=$!
				;;
			*KEY_HELP*'value 0'*)
				[ -n "$tp" ] && kill "$tp" 2>/dev/null
				tp=
				;;
			esac
		done
	}
}

# AVRCP to every connected phone (its player object), and the web
# interface's player. Fails if it reached neither.
media() { # play|pause|next|previous
	case $1 in
	play) m=Play; f=resume ;;
	pause) m=Pause; f=pause ;;
	next) m=Next; f=next ;;
	previous) m=Previous; f=prev ;;
	*) echo "usage: lidar-bluetooth media play|pause|next|previous" >&2; exit 2 ;;
	esac
	n=$(dbus-send --system --print-reply --dest=org.bluez / \
		org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null |
		sed -n 's|.*object path "\(/org/bluez/[^"]*/player[0-9]*\)"$|\1|p' | sort -u |
		while read -r p; do
			dbus-send --system --type=method_call --dest=org.bluez "$p" \
				org.bluez.MediaPlayer1.$m 2>/dev/null && echo "$p"
		done | wc -l)
	to_fifo /run/lidar-player.fifo $f && n=$((n + 1))
	[ "$n" -gt 0 ]
}

# Playback paused for pairing mode. Nothing resumes it.
pause_all() { media pause || :; }

# bluealsa's events:
# - "PCMAdded <pcm>": a phone opened an audio stream. Outside pairing mode
#   only a trusted phone may (the chime); any other is disconnected and
#   removed. The kernel and the agent would let a phone that pairs without
#   bonding through, so trust is the gate. In pairing mode, pairing()
#   decides and plays the chime itself.
# - "PropertyChanged <pcm> Volume 0xLLRR": the phone's volume (left and
#   right, 0-127 each), to lidar-volume
bluealsa_watch() {
	bluealsa-cli monitor --properties=Volume 2>/dev/null | while read -r ev pcm prop val; do
		case $pcm in */a2dpsnk/source) ;; *) continue ;; esac
		case $ev in
		PCMAdded)
			[ -e $PAIRING ] && continue
			m=${pcm#*/dev_}; m=$(echo "${m%%/*}" | tr _ :)
			if has "$m" Trusted; then
				sound paired
			else
				log "refused $m: not paired in pairing mode"
				btctl remove "$m" >/dev/null
			fi
			;;
		PropertyChanged)
			[ "$prop" = Volume ] || continue
			case $val in 0x[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;; *) continue ;; esac
			to_fifo $VOLUME_FIFO "bt $(( (val >> 8) & 127 ))"
			;;
		esac
	done
}

daemon() {
	wait_for 60 bluealsa_up || { no_firmware; log "bluealsa is not running"; sleep 60; exit 1; }
	# phones bonded before they were trusted (older versions of this)
	for m in $(devices_of Bonded); do btctl trust "$m" >/dev/null; done
	# a daemon that died in pairing mode left these behind
	rm -f $PAIRING $STOP
	to_fifo $RING_FIFO "pair off"
	sound_on=$(lidar-feature get bluetooth sound)
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	bt-agent -c NoInputNoOutput 2>&1 | to_kmsg bt-agent &
	# Registering as the default agent makes the adapter pairable: wait for
	# that (or 10 s), then the Echo is neither pairable nor visible until
	# pairing mode
	for i in $(seq 1 20); do
		sleep 0.5
		btctl show | grep -q 'Pairable: yes' && break
	done
	btctl pairable off >/dev/null
	btctl discoverable off >/dev/null
	button_watch &
	bluealsa_watch &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	while read -r cmd <&3; do
		case $cmd in
		toggle) if [ -e $PAIRING ]; then cmd=stop; else cmd=pair; fi ;;
		esac
		case $cmd in
		pair)
			[ -e $PAIRING ] && continue
			rm -f $STOP
			: > $PAIRING
			pairing &
			;;
		stop) [ -e $PAIRING ] && : > $STOP ;;
		esac
	done
}

case $1 in
bluetoothd)
	# the driver loads in the background after Wi-Fi
	wait_for 60 test -d /sys/class/bluetooth/hci0 || { no_firmware; log "no Bluetooth controller"; sleep 60; exit 1; }
	# AVRCP remote-control keys from a phone become Linux input events
	modprobe uinput
	wait_for 30 test -S /run/dbus/system_bus_socket || { log "no D-Bus system bus"; sleep 10; exit 1; }
	e=$(lidar-feature keep /var/lib/bluetooth 2>&1) || { log "${e:-cannot keep /var/lib/bluetooth}"; sleep 60; exit 1; }
	secs=$(lidar-feature get bluetooth pair_seconds)
	# The name as a key-file value: a backslash would start an escape.
	# (bluetoothd also replaces %d and %h in it.)
	name=$(lidar-feature get bluetooth name | sed 's/\\/\\\\/g')
	mkdir -p /etc/bluetooth
	# Class: major Audio/Video, minor Loudspeaker (bluetoothd adds the
	# service bits). JustWorksRepairing: a phone that forgot the Echo can
	# pair again without first removing it here. ReconnectAttempts 0: no
	# reconnecting to a phone after a link loss, only the phone connects.
	cat > /etc/bluetooth/main.conf <<-EOF
	[General]
	Name = $name
	Class = 0x000414
	DiscoverableTimeout = $secs
	PairableTimeout = $secs
	JustWorksRepairing = always
	[Policy]
	AutoEnable = true
	ReconnectAttempts = 0
	EOF
	/usr/lib/bluetooth/bluetoothd -n 2>&1 | to_kmsg bluetoothd
	# the agent registered with the bluetoothd that ended: the daemon
	# starts again with the next one
	pkill -f 'lidar-bluetooth daemon'
	;;
bluealsa)
	wait_for 60 bluetoothd_up || { no_firmware; log "bluetoothd is not running"; sleep 10; exit 1; }
	# Alpine's bluealsa keeps its per-phone state under /usr/var/lib
	mkdir -p /var/lib/bluealsa /usr/var/lib
	ln -sfn /var/lib/bluealsa /usr/var/lib/bluealsa
	# --a2dp-volume: the phone's volume is not applied to the samples here
	# by default (bluealsa-aplay decides, below)
	bluealsa -p a2dp-sink -c aac --a2dp-volume 2>&1 | to_kmsg bluealsa
	;;
aplay)
	wait_for 60 bluealsa_up || { no_firmware; log "bluealsa is not running"; sleep 10; exit 1; }
	# with the volume service, lidar-volume applies the phone's volume as
	# the system volume; without it, bluealsa scales the samples by it
	v=none
	lidar-feature enabled volume || { v=software; log "no volume service: the phone's volume scales the audio"; }
	bluealsa-aplay -D lidar_speaker --volume=$v 2>&1 | to_kmsg bluealsa-aplay
	;;
daemon) daemon ;;
pair)
	if [ ! -p $FIFO ] || ! pgrep -f 'lidar-bluetooth daemon' >/dev/null; then
		echo "lidar-bluetooth daemon is not running (lidar-feature enable bluetooth)" >&2
		exit 1
	fi
	case $2 in
	'') echo pair 1<> $FIFO ;;
	stop) echo stop 1<> $FIFO ;;
	*) echo "usage: lidar-bluetooth pair [stop]" >&2; exit 2 ;;
	esac
	;;
media) media "$2" ;;
status)
	if ! bluetoothd_up >/dev/null; then
		echo "bluetoothd: not running"
		exit 0
	fi
	if [ -e $PAIRING ]; then echo "pairing mode: on"; else echo "pairing mode: off"; fi
	for m in $(devices_of Connected); do
		echo "connected: $(name_of "$m")"
	done
	;;
devices)
	c=" $(devices_of Connected | tr '\n' ' ') "
	btctl devices Paired | while read -r d m name; do
		[ "$d" = Device ] || continue
		s=no; case $c in *" $m "*) s=yes ;; esac
		printf '%s\t%s\t%s\n' "$m" "$s" "$name"
	done
	;;
forget)
	case $2 in
	[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]) ;;
	*) echo "usage: lidar-bluetooth forget <address>" >&2; exit 2 ;;
	esac
	timeout 5 bluetoothctl remove "$2"
	;;
*)
	sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
