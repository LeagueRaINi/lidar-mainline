#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build a bring-up test filesystem for lidar: kernel + DTB + a small Alpine
# initramfs with a root shell on the debug UART (ttyS0, TM58) and on USB
# serial gadget, packed as an ext4 image that U-Boot boots via extlinux.
#
# Runs inside the lidar-build container after `build.sh linux lidar`:
#   build.sh testimg            -> /work/out/lidar-test-system.ext4
#
# Flash with U-Boot fastboot (hold Volume Up at power-on), no UART input needed:
#   fastboot flash system_b out/lidar-test-system.ext4
#   fastboot oem run:"setenv lidar_slot b; boot"
set -euo pipefail
B=${1:-/src/build/lidar}
REL=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64
OUT=/work/out
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mkdir -p "$T/initramfs" "$T/fs/boot/extlinux"
ROOTFS=$(curl -fsSL "$REL/latest-releases.yaml" |
	sed -n 's/^ *file: \(alpine-minirootfs-.*-aarch64\.tar\.gz\)$/\1/p' | head -n1)
[ -n "$ROOTFS" ] || { echo "could not resolve Alpine minirootfs" >&2; exit 1; }
curl -fsSL "$REL/$ROOTFS" | tar -xz -C "$T/initramfs"
python3 /work/tools/alpine-fetch.py "$T/initramfs" alsa-utils i2c-tools evtest iw wpa_supplicant

make -C /src/linux O="$B" -s INSTALL_MOD_PATH="$T/initramfs" INSTALL_MOD_STRIP=1 modules_install
# Out-of-tree MT7668 WLAN driver, built against this exact kernel
KOBJ="$B" MOBJ="$B-wifi-mod" bash /work/lidar-wifi/build.sh modules >/dev/null
KREL=$(cat "$B/include/config/kernel.release")
install -D -m644 "$B-wifi-mod/wlan_mt76x8_sdio.ko" "$T/initramfs/lib/modules/$KREL/extra/wlan_mt76x8_sdio.ko"
depmod -b "$T/initramfs" "$KREL"
# static arm64 build of tools/devmem.c (needs libc6-dev-arm64-cross)
aarch64-linux-gnu-gcc -O2 -static -o "$T/devmem" /work/tools/devmem.c
install -m755 "$T/devmem" "$T/initramfs/usr/bin/devmem"
install -m755 /work/tools/lidar-regdump.sh "$T/initramfs/usr/bin/lidar-regdump"

cat > "$T/initramfs/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /sys/kernel/config && mount -t devpts devpts /dev/pts
mount -t configfs configfs /sys/kernel/config 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
echo "lidar test initramfs: $(uname -r)"
# no udev here: load the sound stack by hand (codecs before the card)
for m in snd-soc-mt8516-afe snd-soc-tlv320adc3xxx snd-soc-tlv320aic32x4-i2c mt8516-amazon-lidar; do
	modprobe $m
done
# USB gadget on the pogo-pin port: serial shell + NCM network (169.254.7.2)
g=/sys/kernel/config/usb_gadget/lidar
if [ -d /sys/kernel/config/usb_gadget ] && [ -n "$(ls /sys/class/udc 2>/dev/null)" ]; then
	mkdir -p $g/strings/0x409 $g/configs/c.1 $g/functions/acm.usb0 $g/functions/ncm.usb0
	echo 0x1d6b > $g/idVendor; echo 0x0104 > $g/idProduct
	echo lidar-mainline > $g/strings/0x409/manufacturer
	echo "Echo Plus 2 (lidar) test" > $g/strings/0x409/product
	ln -s $g/functions/acm.usb0 $g/configs/c.1/
	ln -s $g/functions/ncm.usb0 $g/configs/c.1/
	ls /sys/class/udc | head -n1 > $g/UDC
	(while true; do setsid /bin/sh -l </dev/ttyGS0 >/dev/ttyGS0 2>&1; sleep 1; done) &
	ip link set lo up
	ip addr add 169.254.7.2/16 dev usb0 && ip link set usb0 up
fi
# Stage the owner's own Wi-Fi firmware from Fire OS (system_a, read-only),
# then load WLAN before Bluetooth: btmtksdio must not start while the WLAN
# driver downloads the MT7668 patch. Backgrounded so the shell never waits.
(
	dev=
	for i in $(seq 1 50); do
		p=$(grep -l '^PARTNAME=system_a$' /sys/class/block/*/uevent 2>/dev/null | head -n1)
		[ -n "$p" ] && dev=/dev/$(basename "$(dirname "$p")")
		[ -b "$dev" ] && break
		sleep 0.2
	done
	fw=/mnt/fireos/system/vendor/firmware
	mkdir -p /mnt/fireos /lib/firmware/mediatek
	if [ -b "$dev" ] && mount -o ro -t ext4 "$dev" /mnt/fireos &&
	   cp $fw/*7668* $fw/TxPwrLimit_MT76x8.dat $fw/wifi.cfg /lib/firmware/ &&
	   cp /lib/firmware/mt7668_patch_e2_hdr.bin /lib/firmware/mediatek/mt7668pr2h.bin &&
	   modprobe wlan_mt76x8_sdio && modprobe btmtksdio; then
		echo "lidar: Wi-Fi firmware staged, drivers loaded" > /dev/kmsg
	else
		echo "lidar: Wi-Fi firmware not staged" > /dev/kmsg
	fi
	umount /mnt/fireos 2>/dev/null
) &
exec setsid /bin/sh -l </dev/ttyS0 >/dev/ttyS0 2>&1
EOF
chmod +x "$T/initramfs/init"

(cd "$T/initramfs" && find . | cpio -o -H newc --quiet | gzip -9) > "$T/fs/boot/initramfs.gz"
cp "$B/arch/arm64/boot/Image.gz" "$T/fs/boot/Image.gz"
mkdir -p "$T/fs/boot/dtbs/mediatek"
cp "$B/arch/arm64/boot/dts/mediatek/mt8516-amazon-lidar.dtb" "$T/fs/boot/dtbs/mediatek/"

cat > "$T/fs/boot/extlinux/extlinux.conf" <<'EOF'
default lidar-test
timeout 1

label lidar-test
	kernel /boot/Image.gz
	fdtdir /boot/dtbs
	initrd /boot/initramfs.gz
	append console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000 loglevel=7 clk_ignore_unused
EOF

rm -f "$OUT/lidar-test-system.ext4"
mke2fs -q -t ext4 -L lidar-test -d "$T/fs" "$OUT/lidar-test-system.ext4" 256M
# Loose copies for pushing into a running system's /boot over the USB network
cp "$T/fs/boot/Image.gz" "$T/fs/boot/initramfs.gz" "$OUT/"
cp "$T/fs/boot/dtbs/mediatek/mt8516-amazon-lidar.dtb" "$OUT/"
ls -la "$OUT/lidar-test-system.ext4"
