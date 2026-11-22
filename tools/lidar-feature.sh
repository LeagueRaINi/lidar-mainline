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
#                   allowed, description; passphrases, secrets and hashes
#                   as "(set)")
#        lidar-feature get <name> <key>    (prints the value in effect)
#        lidar-feature set <name> <key> [<value>]
#                  (without <value>: the first line of standard input, which
#                   keeps a secret out of the process list)
#        lidar-feature unset <name> <key>  (back to the default)
#        lidar-feature pending     (changes not yet in effect, one per line:
#                                   feature.<name> or <name>.<key>; compared
#                                   with the file as it was at boot)
#        lidar-feature keep <dir>  (bind <dir> from the booted slot: where
#                                   services keep data across reboots)
#
# Changes take effect on the next boot, except the live settings (see
# is_live): what reads them picks a change up at once.
CONF=${LIDAR_CONF:-/media/system/etc/lidar.conf}

# name category default description
FEATURES='
bootanim core 1 light-ring animation while booting
volume core 1 system volume for the speaker: volume buttons, Bluetooth phones, lidar-volume
ring core 1 LED ring after boot: red while the mics are muted, volume changes, Bluetooth pairing mode
wifi core 1 Wi-Fi client: joins the network in the wifi settings at boot (DHCP); the clock comes from NTP once online. Needs the Wi-Fi firmware from Fire OS or the copy lidar keeps (lidar-fireos)
bluetooth core 1 Bluetooth speaker: phones play on the Echo (A2DP, SBC and AAC), paired by holding the action button 3 s. Needs the Wi-Fi firmware from Fire OS or the copy lidar keeps (lidar-fireos); without the volume service the phone volume scales the samples
dsp core 1 speaker tuning as on Fire OS: loudness EQ by volume, bass EQ, multiband compressor and limiter. Needs the tuning from Fire OS or the copy lidar keeps (lidar-fireos) and the volume service, untuned without them
webui core 1 web interface on port 80 of the USB network: first-run setup, sensors, LED ring, volume, music, Wi-Fi/Bluetooth, services, logs, files
telnet extra 0 root shell without authentication on port 23 of the USB network
zigbee extra 0 Zigbee coordinator: zigbeed on the multi-PAN EFR32 radio, EZSP over TCP port 6638 for Home Assistant (ZHA); runs alongside thread
thread extra 0 Thread border router: otbr-agent on the multi-PAN EFR32 radio, REST API on port 8081 for Home Assistant; runs alongside zigbee
visualizer extra 0 sound display on the LED ring: spectrum or level of whatever the speaker plays, below the displays of the ring service
assistant extra 0 voice assistant: timers, alarms, volume, music, time and date by command (English or German), as text on /run/lidar-assistant.fifo until speech recognition is part of the image. Needs the volume service; alarms and the time need the clock from the network (wifi)
voice extra 0 wake word: listens for "Hey Jarvis" with the beamformer on the seven microphones (lidar-wake), shows the direction on the LED ring and plays a chime; about one CPU core. Silent while the mics are muted
'

# feature key default allowed description
# feature is a service above, or a group of settings without a switch.
# default "-" is unset. allowed is a|b|c (one of), N-M (integer
# range), CC (two capital letters), ssid (1-32 bytes, no control
# characters), label (1-48 bytes, no control characters), passphrase (8-63
# printable ASCII characters, masked in "settings"), secret (16-200
# printable ASCII characters without spaces, masked too), crypt (a
# sha512-crypt hash as cryptpw prints it, "$6$<salt>$<hash>", masked too) or
# tz (a POSIX TZ rule such as CET-1CEST,M3.5.0,M10.5.0/3).
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
dsp bass 0 -6|-5|-4|-3|-2|-1|0|1|2|3|4|5|6 bass in dB on top of the speaker tuning (shelf below 120 Hz); applies at once
dsp mid 0 -6|-5|-4|-3|-2|-1|0|1|2|3|4|5|6 mids in dB on top of the speaker tuning (around 800 Hz); applies at once
dsp treble 0 -6|-5|-4|-3|-2|-1|0|1|2|3|4|5|6 treble in dB on top of the speaker tuning (shelf above 6 kHz); applies at once
dsp boost 0 0-6 loudness boost in dB into the compressor and limiter of the speaker tuning: louder at the same volume, peaks stay limited; applies at once
ring auto_brightness on on|off on: ring brightness follows the light sensor, as stock does; off: always full
ring min_brightness 15 5-100 ring brightness in the dark, percent of full
webui setup_done no no|yes yes: the first-run setup of the web interface was finished; no: the web interface opens it for clients on the USB network
webui auth off off|wifi|all login for the web interface: off: none; wifi: asked of clients that are not on the USB network; all: asked of USB clients too. Set from the web interface (setup, or Security), which stores auth_hash and auth_kind with it
webui auth_kind - pin|password what the login is, for the page to ask for the right thing: pin (digits only, at least 6) or password (at least 10 characters)
webui auth_hash - crypt sha512-crypt hash of the PIN or password of the web interface (cryptpw -m sha512), never shown; without it there is no login, whatever auth says
webui fireos_image_saved no no|yes yes: the owner confirmed in the setup that the full Fire OS image (system_a) was downloaded and saved
voice wake_word hey_jarvis hey_jarvis word the voice assistant listens for (hey_jarvis is the only one for now)
voice language en en|de language the voice assistant understands and speaks: en English, de German
voice timezone UTC0 tz time zone of alarms and the time, as a POSIX TZ string: CET-1CEST,M3.5.0,M10.5.0/3 Central Europe, GMT0BST,M3.5.0/1,M10.5.0 UK, EST5EDT,M3.2.0,M11.1.0 US Eastern
voice ring_volume 30 0-100 volume an alarm or timer rings at, at least, in percent; the speaker is unmuted for it. 0: volume and mute stay as they are
voice voice_de thorsten thorsten|eva_k German voice: thorsten (natural, slower) or eva_k (smaller, about twice as fast); installed by lidar-models
voice sensitivity medium low|medium|high how readily the wake word triggers: low misses more, high triggers more often by mistake
voice api_key - secret Claude API key of the voice assistant, kept in plain text in lidar.conf (readable by root only) and never shown
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

has_settings() { # a service or a group of settings without one
	echo "$SETTINGS" | awk -v f="$1" '$1 == f { found = 1 } END { exit !found }'
}

need_setting() { # feature key; sets DEF and ALLOWED
	has_settings "$1" || need_name "$1"
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
	secret)
		case $2 in *[![:graph:]]*) return 1 ;; esac
		[ ${#2} -ge 16 ] && [ ${#2} -le 200 ] && return 0
		;;
	tz)
		# a POSIX TZ rule (name and offset, e.g. CET-1CEST,...), not a
		# zone file name: the image has no tzdata, and musl would quietly
		# take Europe/Berlin or a bare CET as UTC
		case $2 in '' | *[!A-Za-z0-9+,./:\<\>-]*) return 1 ;; esac
		std=${2%%,*}
		case $std in
		:* | */*) return 1 ;;
		[A-Za-z][A-Za-z][A-Za-z]*[0-9]* | '<'*'>'*[0-9]*) ;;
		*) return 1 ;;
		esac
		[ ${#2} -le 48 ] && return 0
		;;
	crypt)
		case $2 in *[!A-Za-z0-9./\$]*) return 1 ;; esac
		case $2 in '$6$'?*'$'?*) [ ${#2} -le 128 ] && return 0 ;; esac
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

# Settings that apply at once: what reads them does so where it uses them,
# is told (nudge), or the web interface applies them itself (Wi-Fi join,
# login). Everything else takes effect on the next boot.
is_live() { # feature key
	case $1.$2 in
	dsp.* | visualizer.style | ring.min_brightness | \
	volume.sound | volume.ring | bluetooth.sound | bluetooth.pair_seconds | \
	wifi.ssid | wifi.passphrase | voice.api_key | webui.auth* | webui.setup_done | \
	webui.fireos_image_saved) return 0 ;;
	esac
	return 1
}

# -> "", or a note on when the change applies
nudge() { # feature key
	case $1.$2 in
	dsp.*) killall -HUP lidar-dsp 2>/dev/null || echo " (applies when the speaker tuning runs)" ;;
	visualizer.style) killall lidar-viz 2>/dev/null ;;	# init starts it again, with the new style
	wifi.ssid | wifi.passphrase) echo " (applies when Wi-Fi next starts: lidar-wifi restart, or the web interface does it)" ;;
	*) is_live "$1" "$2" || echo " (takes effect on next boot)" ;;
	esac
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
	[ -z "$2" ] || has_settings "$2" || need_name "$2"
	echo "$SETTINGS" | while read -r f k d a desc; do
		[ -n "$f" ] || continue
		[ -z "$2" ] || [ "$f" = "$2" ] || continue
		[ "$d" != - ] || d=
		v=$(setting_value "$f" "$k" "$d" "$a")
		case $a in passphrase | secret | crypt) [ -z "$v" ] || v='(set)' ;; esac
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
	val=$4
	[ $# -ge 4 ] || IFS= read -r val || :
	valid "$ALLOWED" "$val" || {
		case $ALLOWED in
		passphrase) echo "invalid passphrase for $2 $3 (allowed: 8-63 printable ASCII characters)" >&2 ;;
		secret) echo "invalid secret for $2 $3 (allowed: 16-200 printable ASCII characters without spaces)" >&2 ;;
		crypt) echo "invalid hash for $2 $3 (allowed: sha512-crypt, as cryptpw -m sha512 prints it)" >&2 ;;
		tz) echo "invalid time zone '$val' for $2 $3 (allowed: a POSIX TZ rule such as CET-1CEST,M3.5.0,M10.5.0/3 or UTC0, not a zone name)" >&2 ;;
		ssid) echo "invalid value '$val' for $2 $3 (allowed: 1-32 bytes, no control characters)" >&2 ;;
		label) echo "invalid value '$val' for $2 $3 (allowed: 1-48 bytes, no control characters)" >&2 ;;
		*) echo "invalid value '$val' for $2 $3 (allowed: $ALLOWED)" >&2 ;;
		esac
		exit 2
	}
	shown=$val
	case $ALLOWED in passphrase | secret | crypt) shown='(set)' ;; esac
	conf_put "$2.$3" "$val" || exit 1
	echo "$2 $3: $shown$(nudge "$2" "$3")"
	;;
unset)
	need_setting "$2" "$3"
	conf_put "$2.$3" || exit 1
	echo "$2 $3: back to default (${DEF:-unset})$(nudge "$2" "$3")"
	;;
pending)
	# what init saved of the file at boot: a change reverted is no change
	boot=/run/lidar.conf.boot
	[ -r "$boot" ] || exit 0
	now=$CONF; [ -r "$now" ] || now=/dev/null
	# only keys whose last raw value differs can differ in effect
	awk 'FNR == 1 { f++ } { i = index($0, "="); if (!i) next; k = substr($0, 1, i - 1); v[f, k] = substr($0, i + 1); seen[k] = 1 }
		END { for (k in seen) if (!((1, k) in v) || !((2, k) in v) || v[1, k] != v[2, k]) print k }' \
		"$boot" "$now" | sort | while IFS= read -r k; do
		f=${k%%.*}; s=${k#*.}
		if [ "$f" = feature ]; then
			default_of "$s" >/dev/null || continue
			[ "$(value_of "$s")" = "$(CONF=$boot; value_of "$s")" ] || echo "$k"
		else
			da=$(setting_of "$f" "$s") || continue
			is_live "$f" "$s" && continue
			d=${da%% *}; a=${da#* }; [ "$d" != - ] || d=
			[ "$(setting_value "$f" "$s" "$d" "$a")" = "$(CONF=$boot; setting_value "$f" "$s" "$d" "$a")" ] || echo "$k"
		fi
	done
	;;
keep)
	case $2 in /*) ;; *) echo "usage: lidar-feature keep <absolute dir>" >&2; exit 2 ;; esac
	( flock 9 && keep_locked "$2" ) 9> /run/lidar-slot.lock
	;;
*)
	sed -n '3,38p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
