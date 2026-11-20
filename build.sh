#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Run inside the lidar-build container:
#   podman run --rm -v lidar-src:/src -v <work>:/work lidar-build bash /work/build.sh <target> [args]
#
# Source of truth for our changes is /work/lidar-linux and /work/lidar-uboot
# (overlays mirroring the upstream tree layout). They are synced on top of
# pristine checkouts in the lidar-src volume before every build, so the
# trees in the volume are disposable.
#
# O names the kernel build directory /src/build/O (default: lidar).
#
# Targets:
#   fetch                   clone linux v7.3 and u-boot v2026.10 into /src
#   sync                    sync overlays only
#   linux-config [O]        defconfig + lidar.config
#   linux [O] [make args]   build (default: Image.gz dtbs modules)
#   linux-dir O DIR         build one directory, e.g. sound/soc/mediatek/mt8516/
#   uboot                   build U-Boot for lidar and pack lidar-uboot.img
#   bootimg [O]             pack Image.gz + DTB for direct boot by Amazon LK
#   testimg [O]             ext4 test system (kernel, DTB, Alpine initramfs)
#   patches                 export overlays as git patches to /work/out/patches
set -euo pipefail
J=${J:-$(nproc)}
SRC=/src
OUT=/work/out
mkdir -p "$OUT"

sync_tree() { # overlay tree
	local ov=$1 tree=$2 f
	[ -d "$ov" ] || return 0
	rsync -rc --chmod=F644 --exclude '*.orig' "$ov/" "$tree/"
	# Files dropped from the overlay go back to their pristine state.
	git -C "$tree" diff --name-only | while IFS= read -r f; do
		[ -e "$ov/$f" ] || git -C "$tree" checkout -q -- "$f"
	done
	git -C "$tree" ls-files --others --exclude-standard | while IFS= read -r f; do
		[ -e "$ov/$f" ] || rm -f "$tree/$f"
	done
}

sync_all() {
	sync_tree /work/lidar-linux "$SRC/linux"
	sync_tree /work/lidar-uboot "$SRC/u-boot"
}

usage() { sed -n '3,22p' "$0"; exit 1; }

lo() { echo "$SRC/build/${1:-lidar}"; }

# The fragment is the source of truth: reconfigure when it is newer.
linux_config_if_stale() {
	local o; o=$(lo "${1:-}")
	[ -f "$o/.config" ] && ! [ /work/lidar.config -nt "$o/.config" ] || linux_config "${1:-}"
}

need_kernel() { # build dir
	[ -f "$1/arch/arm64/boot/Image.gz" ] ||
		{ echo "run: build.sh linux $(basename "$1") first" >&2; exit 1; }
}

linux_config() {
	local o; o=$(lo "${1:-}")
	make -C "$SRC/linux" O="$o" -s defconfig
	"$SRC/linux/scripts/kconfig/merge_config.sh" -m -O "$o" "$o/.config" /work/lidar.config >/dev/null
	make -C "$SRC/linux" O="$o" -s olddefconfig
	# report fragment options that did not stick
	while IFS= read -r line; do
		case "$line" in
		CONFIG_*=*) grep -qx "$line" "$o/.config" || echo "config: not applied: $line" ;;
		"# CONFIG_"*" is not set")
			sym=${line#\# }; sym=${sym%% *}
			if grep -q "^$sym=" "$o/.config"; then echo "config: still enabled: $sym"; fi ;;
		esac
	done < /work/lidar.config
}

cmd=${1:-}
shift || true
case "$cmd" in
fetch)
	[ -d "$SRC/linux/.git" ] || git clone --depth 1 -b v7.3 \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$SRC/linux"
	[ -d "$SRC/u-boot/.git" ] || git clone --depth 1 -b v2026.10 \
		https://source.denx.de/u-boot/u-boot.git "$SRC/u-boot"
	;;
sync) sync_all ;;
linux-config) sync_all; linux_config "${1:-}" ;;
linux)
	sync_all
	n=${1:-}; shift || true
	o=$(lo "$n")
	linux_config_if_stale "$n"
	[ $# -gt 0 ] || set -- Image.gz dtbs modules
	make -C "$SRC/linux" O="$o" -j"$J" "$@"
	;;
linux-dir)
	[ $# -eq 2 ] || usage
	sync_all
	o=$(lo "$1")
	linux_config_if_stale "$1"
	[ -f "$o/scripts/mod/modpost" ] || make -C "$SRC/linux" O="$o" -j"$J" modules_prepare
	make -C "$SRC/linux" O="$o" -j"$J" W=1 "$2"
	;;
uboot)
	sync_all
	o=$SRC/build/u-boot
	make -C "$SRC/u-boot" O="$o" -s mt8516_amazon_lidar_defconfig
	make -C "$SRC/u-boot" O="$o" -j"$J"
	python3 /work/tools/lidarboot.py pack --kernel "$o/u-boot.bin" \
		--dtb "$o/u-boot.dtb" -o "$OUT/lidar-uboot.img"
	cp "$o/u-boot.bin" "$o/u-boot.dtb" "$OUT/"
	;;
testimg)
	o=$(lo "${1:-}")
	need_kernel "$o"
	bash /work/tools/mk-testimg.sh "$o"
	;;
bootimg)
	# Kernel booted directly by Amazon LK (no U-Boot): Image.gz + DTB in boot_X
	o=$(lo "${1:-}")
	need_kernel "$o"
	python3 /work/tools/lidarboot.py pack --kernel "$o/arch/arm64/boot/Image.gz" \
		--dtb "$o/arch/arm64/boot/dts/mediatek/mt8516-amazon-lidar.dtb" \
		--cmdline "console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000" \
		-o "$OUT/lidar-linux-boot.img"
	;;
patches)
	sync_all
	rm -rf "$OUT/patches"; mkdir -p "$OUT/patches"
	for t in linux u-boot; do
		git -C "$SRC/$t" add -A >/dev/null
		git -C "$SRC/$t" diff --cached --stat
		git -C "$SRC/$t" diff --cached > "$OUT/patches/$t-lidar.diff"
		git -C "$SRC/$t" reset -q
	done
	;;
*) usage ;;
esac
