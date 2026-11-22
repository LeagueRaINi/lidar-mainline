#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Checks the built images and collects the files of a release.
#
# Runs inside the lidar-build container after `build.sh uboot`, `build.sh
# linux` and `build.sh testimg` (.github/workflows/release.yml does this for
# every v* tag):
#   mk-release.sh <version>     e.g. v1.2.0, v1.3.0-rc.1
#
# Output in /work/out/release:
#   lidar-uboot.img, lidar-test-system.ext4   the images, under the names the
#                                             docs/install.md and the installer use
#   lidar-release.txt                         version, kernel, Alpine packages
#   lidar-voicecache-<voice>.tar              the phrase cache of each voice
#   SHA256SUMS
#
# It looks into the system image itself, not at the build's loose copies, and
# refuses to make a release when
#   - /etc/lidar-release in the image is not <version>;
#   - the image contains software that must not be passed on: Silicon Labs'
#     (MSLA: zigbeed, cpcd, libcpc, the RCP image), anything from Fire OS, or
#     firmware other than what mk-testimg.sh fetches from linux-firmware and
#     wireless-regdb;
#   - the image's filesystem holds more than the boot files and a lidar.conf
#     of plain service switches (no network names, keys or logins), or that
#     file turns telnet on.
set -euo pipefail
VER=${1:?usage: mk-release.sh <version>}
OUT=/work/out
REL=$OUT/release
UBOOT=$OUT/lidar-uboot.img
SYSTEM=$OUT/lidar-test-system.ext4

# Semantic version with a leading v; build metadata (+...) is left out, a
# tag cannot be told apart by it
[[ $VER =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$ ]] ||
	{ echo "mk-release: '$VER' is not a version like v1.2.0 or v1.3.0-rc.1" >&2; exit 1; }
for f in "$UBOOT" "$SYSTEM"; do
	[ -s "$f" ] || { echo "mk-release: $f missing (build.sh uboot, linux, testimg)" >&2; exit 1; }
done
# the phrase caches of the voices lidar-models offers (mk-voicecache.sh)
VOICES=$(sh /work/tools/lidar-models.sh -d /tmp/m status | sed -n 's/^available=[^ ]* \([^ ]*\).*/\1/p')
for v in $VOICES; do
	[ -s "$OUT/lidar-voicecache-$v.tar" ] ||
		{ echo "mk-release: $OUT/lidar-voicecache-$v.tar missing (mk-voicecache.sh)" >&2; exit 1; }
done

# a failed check must not leave an older release behind
rm -rf "$REL"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/root"
# -f: debugfs otherwise takes the request from stdin and ends with status 0
# whatever happens
dbg() { echo "$1" > "$W/request"; debugfs -f "$W/request" "$SYSTEM" 2>&1; }
dbg "dump /boot/initramfs.gz $W/initramfs.gz" >/dev/null
dbg "dump /etc/lidar.conf $W/lidar.conf" >/dev/null
[ -s "$W/initramfs.gz" ] || { echo "mk-release: no /boot/initramfs.gz in $SYSTEM" >&2; exit 1; }
gzip -dc "$W/initramfs.gz" | (cd "$W/root" && cpio -idm --quiet --no-absolute-filenames)

{
	# the version the image reports
	have=$(cat "$W/root/etc/lidar-release" 2>/dev/null || true)
	[ "$have" = "$VER" ] ||
		echo "version: /etc/lidar-release is '${have:-missing}', not '$VER' (build testimg with LIDAR_VERSION=$VER)"
	cd "$W/root"
	find . \( -name zigbeed -o -name cpcd -o -name 'libcpc.so*' -o -name '*.gbl' \) \
		-printf 'Silicon Labs software (MSLA): /%P\n'
	find . \( -name 'WIFI_RAM_CODE*' -o -name 'EEPROM_MT7668*' -o -name 'TxPwrLimit*' -o -name wifi.cfg \
		-o -name 'libasp*' -o -path '*/audio-algorithms/*' -o -path './var/lib/lidar-fireos/*' \) \
		-printf 'file from Fire OS: /%P\n'
	# trailing slash: /lib may be a link to usr/lib
	find lib/firmware/ -mindepth 1 ! -type d ! -path lib/firmware/mediatek/mt7668pr2h.bin \
		! -path lib/firmware/mt7668_patch_e2_hdr.bin ! -path lib/firmware/regulatory.db \
		! -path lib/firmware/regulatory.db.p7s -printf 'firmware not on the list: /%p\n'
	[ "$(readlink lib/firmware/mt7668_patch_e2_hdr.bin 2>/dev/null)" = mediatek/mt7668pr2h.bin ] ||
		echo "firmware: /lib/firmware/mt7668_patch_e2_hdr.bin is not the link to linux-firmware's patch"
	# the filesystem around the initramfs
	for d in / /boot /boot/extlinux /boot/dtbs /boot/dtbs/mediatek /etc; do
		dbg "ls -p $d" | awk -F/ -v d="${d%/}" 'NF > 6 && $6 != "." && $6 != ".." { print d "/" $6 }'
	done | grep -v -x -e /boot -e /etc -e /lost+found -e /boot/Image.gz -e /boot/initramfs.gz \
		-e /boot/extlinux -e /boot/extlinux/extlinux.conf -e /boot/dtbs -e /boot/dtbs/mediatek \
		-e /boot/dtbs/mediatek/mt8516-amazon-lidar.dtb -e /etc/lidar.conf |
		sed 's/^/unexpected file in the system image: /' || true
	[ ! -e "$W/lidar.conf" ] || grep -v -x -E 'feature\.[a-z0-9_]+=[01]' "$W/lidar.conf" |
		sed 's/=.*/=.../; s/^/lidar.conf: more than a service switch: /' || true
	! grep -q -x 'feature\.telnet=1' "$W/lidar.conf" 2>/dev/null ||
		echo "lidar.conf: the telnet extra is on"
} > "$W/problems"
if [ -s "$W/problems" ]; then
	sed 's/^/mk-release: /' "$W/problems" >&2
	echo "mk-release: no release made" >&2
	exit 1
fi

mkdir -p "$REL"
cp "$UBOOT" "$SYSTEM" "$REL/"
for v in $VOICES; do cp "$OUT/lidar-voicecache-$v.tar" "$REL/"; done
{
	echo "lidar-mainline $VER"
	[ -z "${LIDAR_COMMIT:-}" ] || echo "commit $LIDAR_COMMIT"
	echo "kernel $(ls "$W/root/lib/modules/")"
	echo "alpine $(cat "$W/root/etc/alpine-release")"
	echo
	echo "Alpine packages in the system image (name version licence):"
	{
		# what tools/alpine-fetch.py installed first: it wins over the
		# minirootfs's own entry for a package it reinstalled
		cat "$W/root/usr/share/lidar/packages" 2>/dev/null || true
		awk -F: '$1 == "P" { p = $2 } $1 == "V" { v = $2 } $1 == "L" { l = $2 }
			/^$/ && p != "" { print p, v, l; p = "" } END { if (p != "") print p, v, l }' \
			"$W/root/lib/apk/db/installed"
	} | awk '!seen[$1]++' | sort
} > "$REL/lidar-release.txt"
(cd "$REL" && sha256sum lidar-uboot.img lidar-test-system.ext4 lidar-release.txt lidar-voicecache-*.tar > SHA256SUMS)
ls -la "$REL"
