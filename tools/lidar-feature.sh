#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Opt-in/opt-out switches for the add-ons on top of the base system, and
# their settings.
#
# Each feature is a U-Boot environment variable lidar_feature_<name> (0 or 1)
# in the "misc" partition, each setting a variable
# lidar_setting_<feature>_<key>. Both slots share them, so a choice survives
# updates. Unset or invalid variables fall back to the defaults below.
#
# Usage: lidar-feature list
#        lidar-feature enabled <name>      (exit status 0 if enabled)
#        lidar-feature enable|disable|reset <name>
#        lidar-feature settings [<name>]
#        lidar-feature get <name> <key>    (prints the value in effect)
#        lidar-feature set <name> <key> <value>
#        lidar-feature unset <name> <key>  (back to the default)
#
# Without Linux: fastboot oem run:"setenv lidar_feature_<name> 0; saveenv"

# name default description
FEATURES='
bootanim 1 light-ring animation while booting
usbaudio 1 USB speakerphone: the Echo appears as a sound card on the USB host
telnet 1 root shell without authentication on port 23 of the USB network
wireless 1 stage Wi-Fi firmware from Fire OS and load the Wi-Fi/BT drivers
'

# feature key default allowed description
# default "-" is unset. allowed is a|b|c (one of), N-M (integer range) or
# CC (two capital letters).
SETTINGS='
telnet listen usb usb|any usb: accept connections on usb0 only; any: on every interface, Wi-Fi included
wireless bluetooth on on|off off: do not load the Bluetooth driver
wireless country - CC regulatory domain (ISO 3166 code), set with "iw reg set" after the driver loads
usbaudio speaker_volume 47 0-127 PCM Playback Volume, 0.5 dB steps, 47 = -40 dB, 127 = 0 dB
usbaudio mic_gain 70 0-80 ADC PGA Capture Volume, 0.5 dB steps, 70 = +35 dB
usbaudio keepalive on on|off on: speaker output stays open (no pops, faint hiss); off: opens per stream
'

default_of() {
	echo "$FEATURES" | awk -v n="$1" '$1 == n { print $2; found = 1 } END { exit !found }'
}

value_of() { # name -> 0/1
	v=$(fw_printenv -n "lidar_feature_$1" 2>/dev/null)
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
	*)
		case $2 in '' | *'|'*) return 1 ;; esac
		case "|$1|" in *"|$2|"*) return 0 ;; esac
		;;
	esac
	return 1
}

setting_value() { # feature key default allowed -> value in effect
	v=$(fw_printenv -n "lidar_setting_$1_$2" 2>/dev/null)
	if valid "$4" "$v"; then echo "$v"; else echo "$3"; fi
}

# On a never-saved environment fw_setenv would write its built-in default,
# which lacks this U-Boot's boot commands.
need_env() {
	fw_printenv -n lidar_slot >/dev/null 2>&1 || {
		echo "U-Boot environment in misc is not initialised; run 'saveenv' in U-Boot first" >&2
		exit 1
	}
}

case $1 in
list)
	echo "$FEATURES" | while read -r n d desc; do
		[ -n "$n" ] || continue
		v=$(value_of "$n")
		s=off; [ "$v" = 1 ] && s=on
		printf '%-9s %-3s (default %s)  %s\n' "$n" "$s" "$( [ "$d" = 1 ] && echo on || echo off)" "$desc"
	done
	;;
enabled)
	need_name "$2"
	[ "$(value_of "$2")" = 1 ]
	;;
enable | disable | reset)
	need_name "$2"
	need_env
	case $1 in
	enable) fw_setenv "lidar_feature_$2" 1 && echo "$2: enabled (takes effect on next boot)" ;;
	disable) fw_setenv "lidar_feature_$2" 0 && echo "$2: disabled (takes effect on next boot)" ;;
	reset) fw_setenv "lidar_feature_$2" && echo "$2: back to default" ;;
	esac
	;;
settings)
	[ -z "$2" ] || need_name "$2"
	echo "$SETTINGS" | while read -r f k d a desc; do
		[ -n "$f" ] || continue
		[ -z "$2" ] || [ "$f" = "$2" ] || continue
		[ "$d" != - ] || d=
		v=$(setting_value "$f" "$k" "$d" "$a")
		printf '%-8s %-14s %-5s (default %s, allowed %s)  %s\n' \
			"$f" "$k" "${v:-unset}" "${d:-unset}" "$a" "$desc"
	done
	;;
get)
	need_setting "$2" "$3"
	setting_value "$2" "$3" "$DEF" "$ALLOWED"
	;;
set)
	need_setting "$2" "$3"
	valid "$ALLOWED" "$4" || {
		echo "invalid value '$4' for $2 $3 (allowed: $ALLOWED)" >&2
		exit 2
	}
	need_env
	fw_setenv "lidar_setting_$2_$3" "$4" && echo "$2 $3: $4 (takes effect on next boot)"
	;;
unset)
	need_setting "$2" "$3"
	need_env
	fw_setenv "lidar_setting_$2_$3" && echo "$2 $3: back to default (${DEF:-unset})"
	;;
*)
	sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
