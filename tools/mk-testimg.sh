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
#   fastboot oem run:"setenv lidar_slot b; saveenv; boot"
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
python3 /work/tools/alpine-fetch.py "$T/initramfs" alsa-utils i2c-tools evtest iw wpa_supplicant \
	wireless-regdb busybox-extras uboot-tools
# fw_printenv/fw_setenv: U-Boot keeps its environment at the top of misc
cat > "$T/initramfs/etc/fw_env.config" <<'EOF'
/dev/disk/by-partlabel/misc 0x70000 0x10000
/dev/disk/by-partlabel/misc 0x60000 0x10000
EOF

make -C /src/linux O="$B" -s INSTALL_MOD_PATH="$T/initramfs" INSTALL_MOD_STRIP=1 modules_install
# Out-of-tree MT7668 WLAN driver, built against this exact kernel
KOBJ="$B" MOBJ="$B-wifi-mod" bash /work/lidar-wifi/build.sh modules >/dev/null
KREL=$(cat "$B/include/config/kernel.release")
install -D -m644 "$B-wifi-mod/wlan_mt76x8_sdio.ko" "$T/initramfs/lib/modules/$KREL/extra/wlan_mt76x8_sdio.ko"
depmod -b "$T/initramfs" "$KREL"
# static arm64 builds of tools/*.c (need libc6-dev-arm64-cross)
aarch64-linux-gnu-gcc -O2 -static -o "$T/devmem" /work/tools/devmem.c
install -m755 "$T/devmem" "$T/initramfs/usr/bin/devmem"
aarch64-linux-gnu-gcc -O2 -Wall -static -o "$T/lidar-listen" /work/tools/lidar-listen.c
install -m755 "$T/lidar-listen" "$T/initramfs/usr/bin/lidar-listen"
install -m755 /work/tools/lidar-regdump.sh "$T/initramfs/usr/bin/lidar-regdump"
install -m755 /work/tools/lidar-bootanim.sh "$T/initramfs/usr/bin/lidar-bootanim"
install -m755 /work/tools/lidar-feature.sh "$T/initramfs/usr/bin/lidar-feature"
install -m755 /work/tools/lidar-usbaudio.sh "$T/initramfs/usr/bin/lidar-usbaudio"

# Named ALSA devices: the speaker, and mics 1-6 summed to mono at 0.5 each
# (+9.5 dB for coherent speech). Mic 7 (capture channel 6) is left out: it
# picks up interference tones on the development unit.
cat > "$T/initramfs/etc/asound.conf" <<'EOF'
# The speaker goes through dmix so a silent keep-alive stream and the USB
# bridge can share it (lidar-usbaudio).
pcm.lidar_dmix {
	type dmix
	ipc_key 2468
	slave {
		pcm "hw:amazonlidar,0"
		format S16_LE
		rate 48000
		channels 2
		period_size 1024
		buffer_size 8192
	}
}
pcm.lidar_speaker {
	type plug
	slave.pcm "lidar_dmix"
}
pcm.lidar_mics_mix {
	type route
	slave { pcm "hw:amazonlidar,1" channels 8 }
	ttable.0.0 0.5
	ttable.0.1 0.5
	ttable.0.2 0.5
	ttable.0.3 0.5
	ttable.0.4 0.5
	ttable.0.5 0.5
}
pcm.lidar_mics {
	type plug
	slave.pcm "lidar_mics_mix"
}
EOF

# Login shells on the debug UART and the USB serial gadget, respawned by
# BusyBox init, which also makes each tty the shell's controlling terminal.
cat > "$T/initramfs/etc/inittab" <<'EOF'
ttyS0::respawn:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

cat > "$T/initramfs/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /sys/kernel/config && mount -t devpts devpts /dev/pts
mount -t configfs configfs /sys/kernel/config 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
# Previous boot's console/oops log, if it was a warm reboot
mount -t pstore pstore /sys/fs/pstore 2>/dev/null
echo "lidar test initramfs: $(uname -r)"
# No udev: name the GPT partitions ourselves. lidar-feature reads its
# switches from the U-Boot environment in misc, so this comes first.
mkdir -p /dev/disk/by-partlabel /run
for i in $(seq 1 25); do
	for u in /sys/class/block/*/uevent; do
		n=$(sed -n 's/^PARTNAME=//p' "$u")
		[ -n "$n" ] && ln -sf "/dev/$(basename "$(dirname "$u")")" "/dev/disk/by-partlabel/$n"
	done
	[ -b /dev/disk/by-partlabel/misc ] && [ -b /dev/disk/by-partlabel/system_a ] && break
	sleep 0.2
done
# Light-ring animation until the last boot job touches /run/boot-done
if lidar-feature enabled bootanim; then
	lidar-bootanim /run/boot-done &
fi
# no udev here: load the sound stack by hand (codecs before the card)
for m in snd-soc-mt8516-afe snd-soc-tlv320adc3xxx snd-soc-tlv320aic32x4-i2c mt8516-amazon-lidar; do
	modprobe $m
done
# Base audio setup: the verified microphone routing (PGA +35 dB); the DAC
# drives both line-out and headphone drivers (the Output Route control picks
# speaker amp or jack) at -6 dB driver gain, speaker volume -40 dB.
A="amixer -q -c amazonlidar cset"
for n in 1 2 3 4; do
	$A name="ADC$n Left Input DIF_2L_3L Capture Switch" on
	$A name="ADC$n Right Input DIF_2R_3R Capture Switch" on
	$A name="ADC$n PGA Capture Switch" on,on
	$A name="ADC$n PGA Capture Volume" 70,70
done
for s in 'LOL Output Mixer L_DAC' 'LOR Output Mixer R_DAC' \
	 'HPL Output Mixer L_DAC' 'HPR Output Mixer R_DAC'; do
	$A name="$s Switch" on
done
$A name='LO DAC Playback Switch' on,on
$A name='HP DAC Playback Switch' on,on
$A name='LO Driver Gain Volume' 0,0
$A name='HP Driver Gain Volume' 0,0
$A name='PCM Playback Volume' 47,47
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
	ip link set lo up
	ip addr add 169.254.7.2/16 dev usb0 && ip link set usb0 up
	# The USB serial console exists now: let init run a shell on it too
	echo 'ttyGS0::respawn:-/bin/sh' >> /etc/inittab
	# Second way in if the serial shell is stuck: telnet, no auth. Unless
	# the owner asks for every interface, the listening socket is tied to
	# usb0 (SO_BINDTODEVICE), so the shell cannot be reached over Wi-Fi.
	# Init owns the bound listener: it restarts it and resets the signals a
	# background job would ignore, so Ctrl-C works in telnet sessions.
	if lidar-feature enabled telnet; then
		if [ "$(lidar-feature get telnet listen)" = any ]; then
			busybox-extras telnetd -l /bin/sh
		else
			echo '::respawn:/usr/bin/lidar-listen usb0 23 busybox-extras telnetd -i -l /bin/sh' \
				>> /etc/inittab
		fi
	fi
fi
# Stage the owner's own Wi-Fi firmware from Fire OS (system_a, read-only),
# then load WLAN before Bluetooth: btmtksdio must not start while the WLAN
# driver downloads the MT7668 patch. The regulatory domain is a cfg80211
# user hint, so it can be set as soon as the driver has pulled cfg80211 in.
# Backgrounded so the shell never waits.
(
	if lidar-feature enabled wireless; then
		dev=/dev/disk/by-partlabel/system_a
		fw=/mnt/fireos/system/vendor/firmware
		mkdir -p /mnt/fireos /lib/firmware/mediatek
		if [ -b "$dev" ] && mount -o ro -t ext4 "$dev" /mnt/fireos &&
		   cp $fw/*7668* $fw/TxPwrLimit_MT76x8.dat $fw/wifi.cfg /lib/firmware/ &&
		   cp /lib/firmware/mt7668_patch_e2_hdr.bin /lib/firmware/mediatek/mt7668pr2h.bin &&
		   modprobe wlan_mt76x8_sdio; then
			echo "lidar: Wi-Fi firmware staged, WLAN driver loaded" > /dev/kmsg
			cc=$(lidar-feature get wireless country)
			if [ -n "$cc" ]; then
				iw reg set "$cc" > /dev/kmsg 2>&1 &&
					echo "lidar: regulatory domain $cc requested" > /dev/kmsg
			fi
			if [ "$(lidar-feature get wireless bluetooth)" = off ]; then
				echo "lidar: Bluetooth driver not loaded (setting)" > /dev/kmsg
			elif ! modprobe btmtksdio; then
				echo "lidar: Bluetooth driver failed to load" > /dev/kmsg
			fi
		else
			echo "lidar: Wi-Fi firmware not staged" > /dev/kmsg
		fi
		umount /mnt/fireos 2>/dev/null
	fi
	if lidar-feature enabled usbaudio; then
		lidar-usbaudio start > /dev/kmsg 2>&1
	fi
	touch /run/boot-done
) &
# BusyBox init becomes PID 1: it runs the shells from /etc/inittab and reaps
# orphaned processes (a plain shell as PID 1 leaves them as zombies).
exec /bin/busybox init
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
