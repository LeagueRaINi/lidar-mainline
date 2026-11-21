#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The files lidar takes from Fire OS. They are not redistributable, so the
# image does not carry them: they come from the owner's Fire OS (system_a,
# read-only), and lidar keeps its own copy on the booted slot. Once that copy
# exists, a wiped system_a no longer matters; an Echo without either gets
# them imported from a backup.
#
#   wifi  the MT7668's Wi-Fi firmware, which Wi-Fi and Bluetooth both need.
#         Required: the two RAM code images. Optional, used when present:
#         the calibration file (the chip's own eFuse calibration is used
#         without it), the per-country power limit table (without it, the
#         kernel's regulatory database alone limits power: set "wifi
#         country") and the driver settings (the driver's defaults
#         otherwise). The ROM patch both functions share is linux-firmware's
#         and part of the image.
#   dsp   Amazon's speaker tuning for this speaker (dsp service): AFE.cfg,
#         EQ_<volume>.cfg, ParametricEQ.cfg, MBCL*.cfg
#
# Usage: lidar-fireos stage wifi|dsp <dir>   at boot (init): refresh the copy
#                                            from Fire OS, put it in <dir>
#        lidar-fireos status                 per file: group, name,
#                                            required|optional, size, sha256
#                                            (first 16), tab-separated; then
#                                            "fireos=yes|no"
#        lidar-fireos export [file]          the copy as a tar archive
#                                            (stdout without a file)
#        lidar-fireos import <tar>           a backup into the copy; takes
#                                            effect on the next boot
DIR=/var/lib/lidar-fireos
FIREOS_DEV=/dev/disk/by-partlabel/system_a
MNT=/run/lidar-fireos.mnt.$$	# per process: two at once must not unmount each other's
WIFI_SRC=system/vendor/firmware
DSP_SRC=system/vendor/etc/audio-algorithms
WIFI_REQUIRED="WIFI_RAM_CODE_MT7668.bin WIFI_RAM_CODE2_SDIO_MT7668.bin"
WIFI_OPTIONAL="EEPROM_MT7668.bin TxPwrLimit_MT76x8.dat wifi.cfg"
MAX=4194304	# bytes per file; the largest (RAM code) is about 450 KB

log() { echo "lidar-fireos: $*" > /dev/kmsg; }
die() { echo "lidar-fireos: $*" >&2; exit 1; }

keep() { lidar-feature keep $DIR >/dev/null 2>&1 && mkdir -p $DIR/wifi $DIR/dsp; }

# a file name of the group, as stored (no paths)
known() { # group name
	case $2 in */* | .*) return 1 ;; esac
	case $1 in
	wifi) case " $WIFI_REQUIRED $WIFI_OPTIONAL " in *" $2 "*) return 0 ;; esac ;;
	dsp) case $2 in AFE.cfg | ParametricEQ.cfg | EQ_[0-9]*.cfg | MBCL*.cfg) return 0 ;; esac ;;
	esac
	return 1
}

complete() { # group dir: everything required there
	case $1 in
	wifi) for f in $WIFI_REQUIRED; do [ -s "$2/$f" ] || return 1; done ;;
	dsp) [ -s "$2/AFE.cfg" ] ;;
	esac
}

# system_a, read-only, on this process's own mountpoint; sets mounted
fireos_mount() {
	mounted=
	mkdir -p $MNT || return 1
	if [ -b $FIREOS_DEV ] && mount -o ro -t ext4 $FIREOS_DEV $MNT 2>/dev/null; then
		mounted=1
		[ -d $MNT/system ] && return 0
		fireos_umount
	fi
	rmdir $MNT 2>/dev/null
	return 1
}

fireos_umount() { # only what this process mounted
	[ -z "$mounted" ] || umount $MNT
	mounted=
	rmdir $MNT 2>/dev/null
}

src_of() { # group -> its directory on Fire OS
	case $1 in wifi) echo $MNT/$WIFI_SRC ;; dsp) echo $MNT/$DSP_SRC ;; esac
}

# the group's files from <from> into the copy, each only if new or changed
# (no writes otherwise); sets changed
store() { # group from
	for p in "$2"/*; do
		f=${p##*/}
		known "$1" "$f" && [ -f "$p" ] && [ ! -L "$p" ] || continue
		cmp -s "$p" "$DIR/$1/$f" && continue
		cp "$p" "$DIR/$1/$f.new" && mv "$DIR/$1/$f.new" "$DIR/$1/$f" || return 1
		changed=1
	done
}

stage() { # group dir
	case $1 in wifi | dsp) ;; *) die "usage: lidar-fireos stage wifi|dsp <dir>" ;; esac
	[ -n "$2" ] && mkdir -p "$2" || die "usage: lidar-fireos stage wifi|dsp <dir>"
	kept=; keep && kept=1
	src=$DIR/$1
	if fireos_mount; then
		if [ -n "$kept" ]; then
			changed=
			store "$1" "$(src_of "$1")" || log "$1: cannot update the copy on the slot"
			[ -z "$changed" ] || log "$1: copy from Fire OS updated"
			# a copy that could not be written: straight from Fire OS
			complete "$1" "$src" || src=$(src_of "$1")
		else
			src=$(src_of "$1")	# nowhere to keep it: straight from Fire OS
		fi
	fi
	if ! complete "$1" "$src"; then
		fireos_umount
		log "$1: no files from Fire OS or a copy (lidar-fireos import <backup>)"
		return 1
	fi
	for p in "$src"/*; do
		known "$1" "${p##*/}" && [ -f "$p" ] && cp "$p" "$2/"
	done
	fireos_umount
	if [ "$src" = "$DIR/$1" ]; then log "$1: from the copy on the slot"; else log "$1: from Fire OS"; fi
}

status() {
	keep || die "booted slot not mounted"
	for f in $WIFI_REQUIRED $WIFI_OPTIONAL; do
		need=optional
		case " $WIFI_REQUIRED " in *" $f "*) need=required ;; esac
		if [ -f "$DIR/wifi/$f" ]; then
			printf 'wifi\t%s\t%s\t%s\t%s\n' "$f" $need "$(wc -c < "$DIR/wifi/$f")" \
				"$(sha256sum "$DIR/wifi/$f" | cut -c1-16)"
		else
			printf 'wifi\t%s\t%s\t\t\n' "$f" $need
		fi
	done
	for p in $DIR/dsp/*; do
		f=${p##*/}
		[ -f "$p" ] && known dsp "$f" || continue	# not a leftover *.new
		need=optional; [ "$f" = AFE.cfg ] && need=required
		printf 'dsp\t%s\t%s\t%s\t%s\n' "$f" $need "$(wc -c < "$p")" "$(sha256sum "$p" | cut -c1-16)"
	done
	[ -f $DIR/dsp/AFE.cfg ] || printf 'dsp\tAFE.cfg\trequired\t\t\n'
	if fireos_mount; then
		echo fireos=yes
		fireos_umount
	else
		echo fireos=no
	fi
}

export_tar() { # [file]
	keep || die "booted slot not mounted"
	complete wifi $DIR/wifi || complete dsp $DIR/dsp || die "nothing to export yet"
	members=
	for g in wifi dsp; do
		for p in $DIR/$g/*; do
			[ -f "$p" ] && known $g "${p##*/}" && members="$members $g/${p##*/}"
		done
	done
	tar -C $DIR -cf "${1:--}" $members
}

import_tar() { # tar file
	[ -f "$1" ] || die "no file $1"
	keep || die "booted slot not mounted"
	# only <group>/<known name> members (and the two directories), and only
	# plain files and directories: no links of any kind (BusyBox lists a
	# hard link as a plain file with "-> target")
	list=$(tar -tf "$1" 2>/dev/null) || die "not a tar archive"
	tar -tvf "$1" 2>/dev/null | cut -c1 | grep -q -v -E '^[-d]$' &&
		die "the archive holds links or special files"
	tar -tvf "$1" 2>/dev/null | grep -q ' -> ' &&
		die "the archive holds links or special files"
	for m in $list; do
		m=${m#./}
		case $m in
		wifi/ | dsp/) continue ;;
		wifi/* | dsp/*) known "${m%%/*}" "${m#*/}" && continue ;;
		esac
		die "unexpected member in the archive: $m"
	done
	t=$(mktemp -d /run/lidar-fireos.XXXXXX) || die "no temporary directory"
	if ! tar -C "$t" -xf "$1"; then rm -rf "$t"; die "cannot unpack $1"; fi
	for g in wifi dsp; do
		for p in "$t/$g"/*; do
			[ -e "$p" ] || [ -L "$p" ] || continue
			[ -f "$p" ] && [ ! -L "$p" ] && [ "$(stat -c %h "$p")" = 1 ] &&
				[ "$(wc -c < "$p")" -le $MAX ] ||
				{ rm -rf "$t"; die "$g/${p##*/}: not a plain file of at most $MAX bytes"; }
		done
	done
	# a wifi copy must be complete afterwards, with RAM code images of a
	# plausible size (about 450 KB and 50 KB on Fire OS)
	if [ -d "$t/wifi" ]; then
		for f in $WIFI_REQUIRED; do
			[ -s "$t/wifi/$f" ] || [ -s "$DIR/wifi/$f" ] ||
				{ rm -rf "$t"; die "the backup lacks wifi/$f"; }
			[ -f "$t/wifi/$f" ] && [ "$(wc -c < "$t/wifi/$f")" -lt 16384 ] &&
				{ rm -rf "$t"; die "wifi/$f is too small to be the firmware"; }
		done
	fi
	# the tuning as it would be afterwards must be one lidar-dsp accepts
	if [ -d "$t/dsp" ]; then
		c=$t/check
		mkdir -p "$c"
		for p in "$DIR/dsp"/* "$t/dsp"/*; do
			[ -f "$p" ] && known dsp "${p##*/}" && cp "$p" "$c/"
		done
		lidar-dsp -c "$c" -k >/dev/null 2>&1 ||
			{ rm -rf "$t"; die "the backup's speaker tuning does not load (lidar-dsp -c <dir> -k)"; }
	fi
	changed=
	for g in wifi dsp; do
		[ -d "$t/$g" ] || continue
		store $g "$t/$g" || { rm -rf "$t"; die "cannot write the copy on the slot"; }
	done
	rm -rf "$t"
	if [ -n "$changed" ]; then
		echo "lidar-fireos: imported; takes effect on the next boot"
	else
		echo "lidar-fireos: the copy already holds these files"
	fi
}

case $1 in
stage) stage "$2" "$3" ;;
status) status ;;
export) export_tar "$2" ;;
import) [ -n "$2" ] || die "usage: lidar-fireos import <tar>"; import_tar "$2" ;;
*)
	sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
