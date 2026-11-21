#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Switches for the services on top of the base system, and their settings.
# Core services make the device work on its own as on stock (volume, LED
# ring, Wi-Fi, Bluetooth speaker, speaker tuning, boot animation) and are
# on by default; a system that brings its own can turn them off. Extras are
# for development and integration (USB, Zigbee, Thread), off by default.
# Services do not require each other (except thread on wlan0, which needs
# wifi): one that misses another falls back (see the descriptions) and logs it.
#
# They live in a plain file, one "key=value" per line:
#   feature.<name>=0|1
#   <name>.<key>=<value>
# By default that is /etc/lidar.conf on the root filesystem of the slot
# that booted (LIDAR_CONF overrides the path). It belongs to the OS, not to
# the bootloader: the U-Boot environment only holds boot state. Missing or
# invalid entries fall back to the defaults below.
#
# Usage: lidar-feature list
#        lidar-feature enabled <name>      (exit status 0 if enabled)
#        lidar-feature enable|disable|reset <name>
#        lidar-feature settings [--raw] [<name>]
#                  (--raw: tab-separated feature, key, value, default,
#                   allowed, description; passphrases as "(set)")
#        lidar-feature get <name> <key>    (prints the value in effect)
#        lidar-feature set <name> <key> <value>
#        lidar-feature unset <name> <key>  (back to the default)
#        lidar-feature keep <dir>  (bind <dir> from the booted slot: where
#                                   services keep data across reboots)
#
# Changes take effect on the next boot.
CONF=${LIDAR_CONF:-/media/system/etc/lidar.conf}

# name category default description
FEATURES='
bootanim core 1 light-ring animation while booting
volume core 1 system volume for the speaker: volume buttons, Bluetooth phones, USB host slider, lidar-volume
ring core 1 LED ring after boot: red while the mics are muted, volume changes, Bluetooth pairing mode
wifi core 1 Wi-Fi client: joins the network in the wifi settings at boot (DHCP); the clock comes from NTP once online. Needs the Wi-Fi firmware from Fire OS or the copy lidar keeps (lidar-fireos)
bluetooth core 1 Bluetooth speaker: phones play on the Echo (A2DP, SBC and AAC), paired by holding the action button 3 s. Needs the Wi-Fi firmware from Fire OS or the copy lidar keeps (lidar-fireos); without the volume service the phone volume scales the samples
dsp core 1 speaker tuning as on Fire OS: loudness EQ by volume, bass EQ, multiband compressor and limiter. Needs the tuning from Fire OS or the copy lidar keeps (lidar-fireos) and the volume service, untuned without them
usbaudio extra 0 USB speakerphone: the Echo appears as a sound card on the USB host
webui extra 0 web interface on port 80 of the USB network: sensors, LED ring, volume, music, Wi-Fi/Bluetooth, services, logs, files
telnet extra 0 root shell without authentication on port 23 of the USB network
zigbee extra 0 Zigbee coordinator: zigbeed on the multi-PAN EFR32 radio, EZSP over TCP port 6638 for Home Assistant (ZHA); runs alongside thread
thread extra 0 Thread border router: otbr-agent on the multi-PAN EFR32 radio, REST API on port 8081 for Home Assistant; runs alongside zigbee
visualizer extra 0 sound display on the LED ring: spectrum or level of whatever the speaker plays, below the displays of the ring service
'

# feature key default allowed description
# default "-" is unset. allowed is a|b|c (one of), N-M (integer range),
# CC (two capital letters), ssid (1-32 bytes, no control characters),
# label (1-48 bytes, no control characters) or passphrase (8-63 printable
# ASCII characters, masked in "settings").
SETTINGS='
telnet listen usb usb|any usb: accept connections on usb0 only; any: on every interface, Wi-Fi included
wifi country - CC regulatory domain (ISO 3166 code), set with "iw reg set" after the driver loads
wifi ssid - ssid Wi-Fi network to join at boot (WPA2-Personal, needs passphrase); unset: no Wi-Fi connection
wifi passphrase - passphrase passphrase of that network, kept in plain text in lidar.conf (readable by root only)
zigbee listen usb usb|any usb: accept the coordinator connection on usb0 only; any: on every interface, Wi-Fi included
thread infra wlan0 wlan0|usb0 network Home Assistant is on: the border router routes between it and the Thread network and advertises there; wlan0 needs the wifi service
thread listen usb usb|any usb: REST API (no authentication) on usb0 only; any: on every interface, Wi-Fi included
bluetooth name lidar label name phones show for the Echo
bluetooth pair_seconds 120 30-600 how long the Echo stays visible and pairable after the action button was held 3 s
bluetooth sound on on|off on: soft sounds while pairing mode runs, a chime when a phone pairs or connects, a falling tone when the mode ends without one
visualizer style level spectrum|level level: arc from LED 1 down both sides, green to red, with a peak mark, longer with louder music and a higher volume; spectrum: 7 bands mirrored, bass at LED 1, treble opposite, red to violet
webui listen usb usb|any usb: accept web connections on usb0 only; any: on every interface, Wi-Fi included
webui storage_mb 256 64-512 RAM for uploaded songs in MB (about 11 minutes per 128 MB), cleared on reboot
volume max_attenuation 0 0-60 cap: full volume is this many dB below the codec 0 dB (0 = Amazon maximum)
volume initial 30 0-100 volume at boot, percent of the range up to the cap
volume ring on on|off on: show volume changes on the LED ring (needs the ring service), as stock does
volume sound on on|off on: a short tone at the new level when the volume buttons change it, as stock does
volume keepalive on on|off on: speaker output stays open, as stock (no clicks, sounds start at once, faint hiss); off: opens per sound
ring auto_brightness on on|off on: ring brightness follows the light sensor, as stock does; off: always full
ring min_brightness 15 5-100 ring brightness in the dark, percent of full
usbaudio mic_gain 70 0-80 ADC PGA Capture Volume, 0.5 dB steps, 70 = +35 dB
usbaudio aec on on|off on: the host gets the mics with the echo of the speaker and background noise removed (lidar-aec); off: the plain mic mix
'

default_of() {
	echo "$FEATURES" | awk -v n="$1" '$1 == n { print $3; found = 1 } END { exit !found }'
}

conf_get() { # key -> last value in the file, empty if none
	[ -r "$CONF" ] || return 0
	awk -v k="$1" 'index($0, k "=") == 1 { v = substr($0, length(k) + 2) } END { print v }' "$CONF"
}

# The file may sit on a filesystem mounted read-only (the test image mounts
# the booted slot that way): open it for writing only around the change. If
# the filesystem itself is writable underneath (services keep their data
# directories there, see keep), only this mount's flag is switched, so the
# data directories stay writable. Changes to the slot's mounts take a lock:
# two of them at once would undo each other's remount.
conf_put() { # key [value]: replace the entry, or remove it without value
	( flock 9 && conf_put_locked "$@" ) 9> /run/lidar-slot.lock
}

conf_put_locked() {
	dir=${CONF%/*}
	# Without the directory the slot is not mounted; writing would land in
	# RAM and be lost at reboot.
	[ -d "$dir" ] || {
		echo "$dir: no such directory (booted slot not mounted? needs lidar.slot=a|b on the kernel command line)" >&2
		return 1
	}
	mnt=$(df -P "$dir" 2>/dev/null | awk 'NR == 2 { print $6 }')
	# mount options and superblock options of the last mount on $mnt
	opts=$(awk -v m="$mnt" '$5 == m { split($0, a, " - "); split(a[2], s, " "); o = $6 " " s[3] }
		END { print o }' /proc/self/mountinfo)
	ro=
	case ",${opts% *}," in *,ro,*)
		case ",${opts#* }," in
		*,ro,*) mount -o remount,rw "$mnt" && ro=remount,ro ;;
		*) mount -o remount,bind,rw "$mnt" && ro=remount,bind,ro ;;
		esac || return 1
		;;
	esac
	# the file holds the Wi-Fi passphrase: readable by root only
	(
		umask 077
		{
			[ -f "$CONF" ] && awk -v k="$1" 'index($0, k "=") != 1' "$CONF"
			[ $# -lt 2 ] || echo "$1=$2"
		} > "$CONF.new"
	) && chmod 600 "$CONF.new" && mv "$CONF.new" "$CONF" && sync
	rc=$?
	[ $rc -eq 0 ] || rm -f "$CONF.new"
	[ -z "$ro" ] || mount -o "$ro" "$mnt"
	return $rc
}

# <dir> of the booted slot, bound to <dir>: the slot's filesystem is made
# writable underneath while its mount at /media/system stays read-only.
keep_locked() {
	slot=/media/system
	mountpoint -q "$1" && return 0
	if [ ! -d $slot/etc ]; then
		echo "booted slot not mounted: nowhere to keep $1" >&2
		return 1
	fi
	mount -o remount,rw $slot || return 1
	mkdir -p "$slot$1" && chmod 700 "$slot$1"
	ok=$?
	# back to read-only for this mount only: the filesystem stays writable
	mount -o remount,bind,ro $slot || return 1
	[ $ok -eq 0 ] &&
		mkdir -p "$1" &&
		mount --bind "$slot$1" "$1" &&
		mount -o remount,bind,rw "$1"
}

value_of() { # name -> 0/1
	v=$(conf_get "feature.$1")
	case $v in
	0 | 1) echo "$v" ;;
	*) default_of "$1" ;;
	esac
}

need_name() {
	[ -n "$1" ] && default_of "$1" >/dev/null || {
		echo "unknown feature '$1' (see: lidar-feature list)" >&2
		exit 2
	}
}

setting_of() { # feature key -> "default allowed"
	echo "$SETTINGS" | awk -v f="$1" -v k="$2" \
		'$1 == f && $2 == k { print $3, $4; found = 1 } END { exit !found }'
}

need_setting() { # feature key; sets DEF and ALLOWED
	need_name "$1"
	s=$(setting_of "$1" "$2") && [ -n "$2" ] || {
		echo "unknown setting '$2' of $1 (see: lidar-feature settings $1)" >&2
		exit 2
	}
	DEF=${s%% *}
	ALLOWED=${s#* }
	[ "$DEF" != - ] || DEF=
}

valid() { # allowed value
	case $1 in
	CC)
		case $2 in [A-Z][A-Z]) return 0 ;; esac
		;;
	[0-9]*-[0-9]*)
		# no leading zeros: amixer would read them as octal
		case $2 in '' | *[!0-9]* | ?????* | 0?*) return 1 ;; esac
		[ "$2" -ge "${1%-*}" ] && [ "$2" -le "${1#*-}" ] && return 0
		;;
	ssid | label)
		case $2 in *[[:cntrl:]]*) return 1 ;; esac
		n=$(printf %s "$2" | wc -c)
		max=32; [ "$1" = label ] && max=48
		[ "$n" -ge 1 ] && [ "$n" -le $max ] && return 0
		;;
	passphrase)
		case $2 in *[![:print:]]*) return 1 ;; esac
		[ ${#2} -ge 8 ] && [ ${#2} -le 63 ] && return 0
		;;
	*)
		case $2 in '' | *'|'*) return 1 ;; esac
		case "|$1|" in *"|$2|"*) return 0 ;; esac
		;;
	esac
	return 1
}

setting_value() { # feature key default allowed -> value in effect
	v=$(conf_get "$1.$2")
	if valid "$4" "$v"; then printf '%s\n' "$v"; else printf '%s\n' "$3"; fi
}

case $1 in
list)
	echo "$FEATURES" | while read -r n c d desc; do
		[ -n "$n" ] || continue
		v=$(value_of "$n")
		s=off; [ "$v" = 1 ] && s=on
		printf '%-9s %-5s %-3s (default %s)  %s\n' "$n" "$c" "$s" "$( [ "$d" = 1 ] && echo on || echo off)" "$desc"
	done
	;;
enabled)
	need_name "$2"
	[ "$(value_of "$2")" = 1 ]
	;;
enable | disable | reset)
	need_name "$2"
	case $1 in
	enable) conf_put "feature.$2" 1 && echo "$2: enabled (takes effect on next boot)" ;;
	disable) conf_put "feature.$2" 0 && echo "$2: disabled (takes effect on next boot)" ;;
	reset) conf_put "feature.$2" && echo "$2: back to default" ;;
	esac
	;;
settings)
	raw=
	[ "$2" != --raw ] || { raw=1; shift; }
	[ -z "$2" ] || need_name "$2"
	echo "$SETTINGS" | while read -r f k d a desc; do
		[ -n "$f" ] || continue
		[ -z "$2" ] || [ "$f" = "$2" ] || continue
		[ "$d" != - ] || d=
		v=$(setting_value "$f" "$k" "$d" "$a")
		[ "$a" != passphrase ] || [ -z "$v" ] || v='(set)'
		if [ -n "$raw" ]; then
			printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$f" "$k" "$v" "$d" "$a" "$desc"
		else
			printf '%-8s %-15s %-5s (default %s, allowed %s)  %s\n' \
				"$f" "$k" "${v:-unset}" "${d:-unset}" "$a" "$desc"
		fi
	done
	;;
get)
	need_setting "$2" "$3"
	setting_value "$2" "$3" "$DEF" "$ALLOWED"
	;;
set)
	need_setting "$2" "$3"
	valid "$ALLOWED" "$4" || {
		case $ALLOWED in
		passphrase) echo "invalid passphrase for $2 $3 (allowed: 8-63 printable ASCII characters)" >&2 ;;
		ssid) echo "invalid value '$4' for $2 $3 (allowed: 1-32 bytes, no control characters)" >&2 ;;
		label) echo "invalid value '$4' for $2 $3 (allowed: 1-48 bytes, no control characters)" >&2 ;;
		*) echo "invalid value '$4' for $2 $3 (allowed: $ALLOWED)" >&2 ;;
		esac
		exit 2
	}
	shown=$4
	[ "$ALLOWED" != passphrase ] || shown='(set)'
	conf_put "$2.$3" "$4" && echo "$2 $3: $shown (takes effect on next boot)"
	;;
unset)
	need_setting "$2" "$3"
	conf_put "$2.$3" && echo "$2 $3: back to default (${DEF:-unset})"
	;;
keep)
	case $2 in /*) ;; *) echo "usage: lidar-feature keep <absolute dir>" >&2; exit 2 ;; esac
	( flock 9 && keep_locked "$2" ) 9> /run/lidar-slot.lock
	;;
*)
	sed -n '3,31p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
