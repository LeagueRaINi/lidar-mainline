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
	wireless-regdb busybox-extras uboot-tools webrtc-audio-processing-2 socat libgpiod cjson \
	alsa-ucm-conf bluez-btmgmt bluez dbus bluez-alsa bluez-alsa-utils bluez-tools
# Package scripts do not run here: the system bus's own user, which dbus'
# install script would add
grep -q '^messagebus:' "$T/initramfs/etc/passwd" ||
	echo 'messagebus:x:100:101:messagebus:/run/dbus:/sbin/nologin' >> "$T/initramfs/etc/passwd"
grep -q '^messagebus:' "$T/initramfs/etc/group" ||
	echo 'messagebus:x:101:messagebus' >> "$T/initramfs/etc/group"
# fw_printenv/fw_setenv: U-Boot keeps its environment at the top of misc
cat > "$T/initramfs/etc/fw_env.config" <<'EOF'
/dev/disk/by-partlabel/misc 0x70000 0x10000
/dev/disk/by-partlabel/misc 0x60000 0x10000
EOF
# MT7668 ROM patch from linux-firmware (redistributable), for Bluetooth and
# Wi-Fi alike: under mainline btmtksdio the Bluetooth firmware crashes on
# Amazon's newer patch (2025 build) at its first LE or vendor command.
LF=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
mkdir -p "$T/initramfs/lib/firmware/mediatek"
curl -fsSL "$LF/mediatek/mt7668pr2h.bin?h=20261019" -o "$T/initramfs/lib/firmware/mediatek/mt7668pr2h.bin"
echo "465ed634c61b46b34bb21240736f33d3d347fefec7cbfbd1cb6e63334aec4cc9  $T/initramfs/lib/firmware/mediatek/mt7668pr2h.bin" |
	sha256sum -c --quiet
# the WLAN driver asks for the same patch under its own name
ln -s mediatek/mt7668pr2h.bin "$T/initramfs/lib/firmware/mt7668_patch_e2_hdr.bin"

make -C /src/linux O="$B" -s INSTALL_MOD_PATH="$T/initramfs" INSTALL_MOD_STRIP=1 modules_install
# Out-of-tree MT7668 WLAN driver, built against this exact kernel
KOBJ="$B" MOBJ="$B-wifi-mod" bash /work/lidar-wifi/build.sh modules >/dev/null
KREL=$(cat "$B/include/config/kernel.release")
install -D -m644 "$B-wifi-mod/wlan_mt76x8_sdio.ko" "$T/initramfs/lib/modules/$KREL/extra/wlan_mt76x8_sdio.ko"
depmod -b "$T/initramfs" "$KREL"
# Keep only the modules this board uses (tools/initramfs-modules.txt) and
# what they depend on; then index again.
python3 - "$T/initramfs/lib/modules/$KREL" /work/tools/initramfs-modules.txt <<'EOF'
import os, re, sys
mdir, listfile = sys.argv[1], sys.argv[2]
norm = lambda n: os.path.basename(n).split(".ko")[0].replace("-", "_")
deps, path = {}, {}
for line in open(os.path.join(mdir, "modules.dep")):
    mod, rest = line.split(":", 1)
    path[norm(mod)] = mod
    deps[norm(mod)] = [norm(d) for d in rest.split()]
for line in open(os.path.join(mdir, "modules.softdep")):
    m = re.match(r"softdep (\S+) (.*)", line)
    if m:
        deps.setdefault(norm(m[1]), []).extend(
            norm(w) for w in m[2].split() if not w.endswith(":"))
want = [norm(l.strip()) for l in open(listfile) if l.strip() and not l.strip().startswith("#")]
missing = [w for w in want if w not in path]
if missing:
    sys.exit("initramfs-modules.txt: not built: " + " ".join(missing))
whole = ("kernel/net/", "kernel/crypto/", "kernel/lib/", "kernel/arch/", "extra/")
# the whole-kept modules need their dependencies too
keep, todo = set(), list(want) + [m for m, p in path.items() if p.startswith(whole)]
while todo:
    m = todo.pop()
    if m in path and m not in keep:
        keep.add(m)
        todo += deps.get(m, [])
removed = 0
for m, p in path.items():
    if m not in keep and not p.startswith(whole):
        os.remove(os.path.join(mdir, p))
        removed += 1
for root, dirs, files in os.walk(mdir, topdown=False):
    if root != mdir and not os.listdir(root):
        os.rmdir(root)
print(f"modules: kept {len(path) - removed} of {len(path)}")
EOF
depmod -b "$T/initramfs" "$KREL"
# static arm64 builds of tools/*.c (need libc6-dev-arm64-cross)
aarch64-linux-gnu-gcc -O2 -static -o "$T/devmem" /work/tools/devmem.c
install -m755 "$T/devmem" "$T/initramfs/usr/bin/devmem"
aarch64-linux-gnu-gcc -O2 -Wall -static -o "$T/lidar-listen" /work/tools/lidar-listen.c
install -m755 "$T/lidar-listen" "$T/initramfs/usr/bin/lidar-listen"
# lidar-aec and lidar-dsp link against Alpine libraries: clang builds them
# for musl against an Alpine sysroot, cached per Alpine release in /src and
# rebuilt when the package list changes.
SYSROOT=/src/alpine-sysroot-$(cat "$T/initramfs/etc/alpine-release")
SYSPKGS="musl-dev libstdc++-dev gcc alsa-lib-dev webrtc-audio-processing-2-dev cjson-dev"
if [ "$(cat "$SYSROOT/.complete" 2>/dev/null)" != "$SYSPKGS" ]; then
	rm -rf "$SYSROOT"
	mkdir -p "$SYSROOT"
	curl -fsSL "$REL/$ROOTFS" | tar -xz -C "$SYSROOT"
	python3 /work/tools/alpine-fetch.py "$SYSROOT" $SYSPKGS
	echo "$SYSPKGS" > "$SYSROOT/.complete"
fi
clang++ --target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld \
	-std=c++17 -O2 -Wall -Wextra -Wno-nullability-completeness \
	-isystem "$SYSROOT/usr/include/webrtc-audio-processing-2" \
	-DWEBRTC_POSIX -DWEBRTC_LIBRARY_IMPL -s -o "$T/lidar-aec" /work/tools/lidar-aec.cc \
	-lwebrtc-audio-processing-2 -lasound
install -m755 "$T/lidar-aec" "$T/initramfs/usr/bin/lidar-aec"
clang --target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld \
	-std=gnu11 -O2 -ffp-contract=fast -Wall -Wextra -Wno-nullability-completeness \
	-s -o "$T/lidar-dsp" /work/tools/lidar-dsp.c -lcjson -lasound -lm
install -m755 "$T/lidar-dsp" "$T/initramfs/usr/bin/lidar-dsp"
aarch64-linux-gnu-gcc -O2 -Wall -static -o "$T/lidar-xmodem" /work/tools/lidar-xmodem.c
install -m755 "$T/lidar-xmodem" "$T/initramfs/usr/bin/lidar-xmodem"
clang --target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld \
	-std=gnu11 -O2 -Wall -Wextra -Wno-nullability-completeness \
	-s -o "$T/lidar-player" /work/tools/lidar-player.c -lasound
install -m755 "$T/lidar-player" "$T/initramfs/usr/bin/lidar-player"
clang --target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld \
	-std=gnu11 -O2 -ffp-contract=fast -Wall -Wextra -Wno-nullability-completeness \
	-s -o "$T/lidar-viz" /work/tools/lidar-viz.c -lasound -lm
install -m755 "$T/lidar-viz" "$T/initramfs/usr/bin/lidar-viz"
install -D -m644 /work/tools/webui/index.html "$T/initramfs/usr/share/lidar/www/index.html"
install -D -m755 /work/tools/webui/api.cgi "$T/initramfs/usr/share/lidar/www/cgi-bin/api"
install -m755 /work/tools/lidar-radio.sh "$T/initramfs/usr/bin/lidar-radio"
install -m755 /work/tools/lidar-bluetooth.sh "$T/initramfs/usr/bin/lidar-bluetooth"
install -m755 /work/tools/lidar-regdump.sh "$T/initramfs/usr/bin/lidar-regdump"
install -m755 /work/tools/lidar-bootanim.sh "$T/initramfs/usr/bin/lidar-bootanim"
install -m755 /work/tools/lidar-feature.sh "$T/initramfs/usr/bin/lidar-feature"
install -m755 /work/tools/lidar-usbaudio.sh "$T/initramfs/usr/bin/lidar-usbaudio"
install -m755 /work/tools/lidar-volume.sh "$T/initramfs/usr/bin/lidar-volume"
install -m755 /work/tools/lidar-ring.sh "$T/initramfs/usr/bin/lidar-ring"
install -m755 /work/tools/lidar-efr32.sh "$T/initramfs/usr/bin/lidar-efr32"
install -m755 /work/tools/lidar-wifi.sh "$T/initramfs/usr/bin/lidar-wifi"
install -m755 /work/tools/lidar-time.sh "$T/initramfs/usr/bin/lidar-time"
# Thread border router: ot-br-posix, pinned, built for musl against the
# sysroot like the tools above. Its sources (with submodules) are cached in
# /src. The vendor and product name end up in the border agent's mDNS
# record; -Wno-error=sign-compare because musl's CMSG_NXTHDR trips clang's
# -Wsign-compare. No ingress firewall: the default one drives the ipset and
# ip6tables tools with rules from ot-br-posix's setup scripts, none of which
# this system has (OTBR_NFTABLES would be the in-process alternative).
#
# The radio is shared with Zigbee (multiprotocol RCP), so otbr-agent does
# not own the UART: cpcd does, and otbr-agent reaches the RCP through
# libcpc with the Gecko SDK's CPC Spinel interface, plugged in as
# OpenThread's vendor bus. Radio URL: spinel+cpc://cpcd_0?iid=2&iid-list=0
# (OT_MULTIPAN_RCP is what makes the URL parser take iid and iid-list).
# cpc-daemon is built as a subdirectory of the same CMake build, without
# link encryption, which drops its mbedtls dependency: cpcd itself and
# libcpc.so.3, which otbr-agent needs at run time. Both keep cpc-daemon's
# default socket folder, /dev/shm, which has to exist before cpcd starts.
# cpc-daemon is under Silicon Labs' MSLA, like zigbeed and the RCP below:
# this image is for your own use, not for passing on.
CPCD_TAG=v4.4.6
CPCD_COMMIT=5e10b3e45ace73058327425a3fbedf06b4bcccbb
CPCD=/src/cpc-daemon
if [ "$(cat $CPCD/.complete 2>/dev/null)" != $CPCD_COMMIT ]; then
	rm -rf $CPCD
	git -c advice.detachedHead=false clone -q --depth 1 --branch $CPCD_TAG https://github.com/SiliconLabs/cpc-daemon.git $CPCD
	[ "$(git -C $CPCD rev-parse HEAD)" = $CPCD_COMMIT ] ||
		{ echo "cpc-daemon $CPCD_TAG is no longer $CPCD_COMMIT" >&2; exit 1; }
	echo $CPCD_COMMIT > $CPCD/.complete
fi
# Only the POSIX host glue of the Gecko SDK (v4.4.3, the release the RCP
# firmware is built from): a sparse, blobless checkout of that directory.
GSDK_COMMIT=d95099f4c05b3c40cece93d8a261dd471e642086
GSDK=/src/gecko_sdk
GSDK_POSIX=protocol/openthread/platform-abstraction/posix
if [ "$(cat $GSDK/.complete 2>/dev/null)" != $GSDK_COMMIT ]; then
	rm -rf $GSDK
	git init -q $GSDK
	git -C $GSDK remote add origin https://github.com/SiliconLabs/gecko_sdk.git
	git -C $GSDK sparse-checkout set --no-cone /$GSDK_POSIX/
	git -C $GSDK fetch -q --depth 1 --filter=blob:none origin $GSDK_COMMIT
	git -C $GSDK -c advice.detachedHead=false checkout -q FETCH_HEAD
	echo $GSDK_COMMIT > $GSDK/.complete
fi
OTBR_TAG=v2026.11.0
OTBR_COMMIT=76fb21d666a41050b43f8eaabc30e71d9d75d9fd
OTBR=/src/ot-br-posix
if [ "$(cat $OTBR/.complete 2>/dev/null)" != $OTBR_COMMIT ]; then
	rm -rf $OTBR
	git clone -q --depth 1 --branch $OTBR_TAG https://github.com/openthread/ot-br-posix.git $OTBR
	[ "$(git -C $OTBR rev-parse HEAD)" = $OTBR_COMMIT ] ||
		{ echo "ot-br-posix $OTBR_TAG is no longer $OTBR_COMMIT" >&2; exit 1; }
	git -C $OTBR submodule update -q --init --recursive --depth 1
	echo $OTBR_COMMIT > $OTBR/.complete
fi
# kept between runs, so ninja only rebuilds what changed
OB=/src/build/otbr-$OTBR_COMMIT-${SYSROOT##*/}
mkdir -p "$OB"
# cpc_interface.cpp was written for a 2024 OpenThread. What this one needs
# changed, applied to a copy in the build directory:
#  - US_PER_S is now OT_US_PER_S;
#  - BigEndian::ReadUint16() left the Encoding namespace, and its header is
#    no longer pulled in by the other includes;
#  - OT_ASSERT() is not declared without common/debug.hpp and compiles to
#    nothing in this build, which leaves a set-but-unused variable (an error
#    here) and failed endpoint options unnoticed: exit on them instead.
# The copy is only replaced when it changes, to keep the build incremental.
mkdir -p "$OB/cpc-vendor"
sed -e 's/\bUS_PER_S\b/OT_US_PER_S/g' \
	-e 's/Encoding::BigEndian::/BigEndian::/' \
	-e 's|^#include "common/code_utils.hpp"|&\n#include "common/encoding.hpp"|' \
	-e 's/OT_ASSERT(\(.*\));/VerifyOrDie(\1, OT_EXIT_FAILURE);/' \
	$GSDK/$GSDK_POSIX/cpc_interface.cpp > "$T/cpc_interface.cpp"
[ "$(grep -c -e OT_US_PER_S -e ' BigEndian::' -e common/encoding.hpp -e 'VerifyOrDie(.*OT_EXIT_FAILURE' \
	"$T/cpc_interface.cpp")" = 8 ] || { echo "cpc_interface.cpp: the OpenThread port no longer applies" >&2; exit 1; }
for f in "$T/cpc_interface.cpp" $GSDK/$GSDK_POSIX/cpc_interface.hpp $GSDK/$GSDK_POSIX/posix_vendor_rcp.cmake; do
	cmp -s "$f" "$OB/cpc-vendor/${f##*/}" || cp "$f" "$OB/cpc-vendor/"
done
[ -f "$OB/toolchain.cmake" ] || cat > "$OB/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -s")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
# The toolchain file predates the shared libcpc, hence the linker flags
# here. -Wno-error=cpp: cpc-daemon includes <sys/errno.h>, a #warning in
# musl, and inherits OpenThread's -Werror. The vendor bus's URL scheme is a
# plain define without a CMake option.
cmake -S $OTBR -B "$OB" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$OB/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=MinSizeRel "-DCMAKE_C_FLAGS=-Wno-error=sign-compare -Wno-error=cpp" \
	'-DCMAKE_CXX_FLAGS=-Wno-error=sign-compare -DOPENTHREAD_POSIX_CONFIG_SPINEL_VENDOR_INTERFACE_URL_PROTOCOL_NAME=\"spinel+cpc\"' \
	"-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -s" -DBUILD_TESTING=OFF \
	-DOTBR_VENDOR_NAME=lidar-mainline "-DOTBR_PRODUCT_NAME=Thread Border Router" \
	-DOTBR_REST=ON -DOTBR_NAT64=OFF -DOTBR_NOTIFY_UPSTART=OFF -DOT_FIREWALL=OFF \
	-DOT_MULTIPAN_RCP=ON -DOT_POSIX_RCP_VENDOR_BUS=ON \
	-DOT_POSIX_CONFIG_RCP_VENDOR_INTERFACE="$OB/cpc-vendor/cpc_interface.cpp" \
	-DOT_POSIX_CONFIG_RCP_VENDOR_DEPS_PACKAGE="$OB/cpc-vendor/posix_vendor_rcp.cmake" \
	-DCPCD_SOURCE_DIR=$CPCD -DENABLE_ENCRYPTION=OFF -DCPCD_CONFIG_FILE_PATH=/etc/cpcd.conf > "$T/otbr.log" 2>&1 &&
	cmake --build "$OB" >> "$T/otbr.log" 2>&1 || { tail -n 40 "$T/otbr.log" >&2; exit 1; }
install -m755 "$OB/src/agent/otbr-agent" "$T/initramfs/usr/bin/otbr-agent"
install -m755 "$OB/third_party/openthread/repo/src/posix/ot-ctl" "$T/initramfs/usr/bin/ot-ctl"
# posix_vendor_rcp.cmake adds cpc-daemon as subdirectory "build"
CB="$OB/third_party/openthread/repo/src/posix/platform/build"
install -m755 "$CB/cpcd" "$T/initramfs/usr/bin/cpcd"
install -m755 "$CB/libcpc.so.3" "$T/initramfs/usr/lib/libcpc.so.3"
# cpcd's configuration; what is not set here keeps cpc-daemon's default.
# The radio's UART has no RTS/CTS and the link is not encrypted.
cat > "$T/initramfs/etc/cpcd.conf" <<'EOF'
instance_name: cpcd_0
bus_type: UART
uart_device_file: /dev/ttyS2
uart_device_baud: 115200
uart_hardflow: false
disable_encryption: true
stdout_trace: false
EOF
# The multiprotocol RCP image comes from efr32/build.sh and zigbeed, the
# Zigbee host stack, from efr32/zigbeed.sh (their own container)
RCP=/work/out/efr32/lidar-mp-rcp.gbl
if [ -f $RCP ]; then
	install -D -m644 $RCP "$T/initramfs/usr/share/lidar/efr32/lidar-mp-rcp.gbl"
else
	echo "warning: $RCP missing (build it with efr32/build.sh): the radio cannot be flashed" >&2
fi
ZIGBEED=/work/out/zigbeed/zigbeed
if [ -f $ZIGBEED ]; then
	install -m755 $ZIGBEED "$T/initramfs/usr/bin/zigbeed"
else
	echo "warning: $ZIGBEED missing (build it with efr32/zigbeed.sh): no Zigbee" >&2
fi
# Feedback sounds in the speaker's native format (48 kHz, 16 bit stereo).
# - volume-tone (lidar-volume): 90 ms at 520 Hz, -10 dBFS, 20 ms
#   raised-cosine fades
# - bt-* (lidar-bluetooth): bell notes, a fundamental with three fading
#   overtones, 4 ms attack and an exponential decay; notes (Hz, start s)
#   may overlap. bt-pairing: two short A5, repeated in pairing mode;
#   bt-paired: C6 E6 G6 C7 rising; bt-timeout: G5 C5 falling
mkdir -p "$T/initramfs/usr/share/lidar"
python3 - "$T/initramfs/usr/share/lidar" <<'EOF'
import math, struct, sys, wave
fs = 48000
def save(name, x):
    with wave.open(f"{sys.argv[1]}/{name}.wav", "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes(b"".join(struct.pack("<hh", s, s) for s in (int(v * 32767) for v in x)))
def tone(f, ms, db, fade_ms):
    a, n, fade = 10 ** (db / 20), fs * ms // 1000, fs * fade_ms // 1000
    return [a * (0.5 - 0.5 * math.cos(math.pi * min(1.0, i / fade, (n - 1 - i) / fade)))
            * math.sin(2 * math.pi * f * i / fs) for i in range(n)]
def bell(notes, tau, length, db):
    n, out = int(length * fs), [0.0] * int(length * fs)
    for f, start in notes:
        s = int(start * fs)
        for i in range(n - s):
            t = i / fs
            env = min(t / 0.004, 1.0) * math.exp(-t / tau)
            out[s + i] += env * sum(a * math.exp(-t * (k - 1) * 3) * math.sin(2 * math.pi * f * k * t)
                                    for k, a in ((1, 1.0), (2, 0.45), (3, 0.2), (4.2, 0.08)))
    fade = fs // 50
    for i in range(fade):
        out[n - fade + i] *= 1 - i / fade
    g = 10 ** (db / 20) / max(abs(v) for v in out)
    return [v * g for v in out]
save("volume-tone", tone(520, 90, -10, 20))
save("bt-pairing", bell([(880, 0), (880, 0.16)], 0.09, 0.6, -10))
save("bt-paired", bell([(1047, 0), (1319, 0.1), (1568, 0.2), (2093, 0.3)], 0.3, 1.3, -4))
save("bt-timeout", bell([(784, 0), (523, 0.22)], 0.25, 1.0, -8))
EOF

# The card's UCM profile (alsa-ucm-conf layout)
U="$T/initramfs/usr/share/alsa/ucm2"
install -D -m644 -t "$U/MediaTek/amazon-lidar" /work/ucm2/MediaTek/amazon-lidar/*.conf
mkdir -p "$U/conf.d/amazon-lidar"
ln -sf ../../MediaTek/amazon-lidar/amazon-lidar.conf "$U/conf.d/amazon-lidar/amazon-lidar.conf"

# Named ALSA devices: the speaker, and mics 1-6 summed to mono at 0.5 each
# (+9.5 dB for coherent speech). Mic 7 (capture channel 6) is left out: it
# picks up narrow interference tones.
cat > "$T/initramfs/etc/asound.conf" <<'EOF'
# The speaker goes through dmix so lidar-volume's silent keep-alive stream,
# its feedback tone and the USB bridge can share it.
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

# With the speaker tuning (dsp feature) the mix goes into snd-aloop instead,
# for lidar-dsp to process. The loopback is clocked by the codec's period
# ticks, so dmix uses lidar-dsp's period (384 frames); the buffer stays
# about as long as above.
sed -e 's|pcm "hw:amazonlidar,0"|pcm "hw:Loopback,0,0"|' \
    -e 's|period_size 1024|period_size 384|' \
    -e 's|buffer_size 8192|buffer_size 7680|' \
	"$T/initramfs/etc/asound.conf" > "$T/initramfs/usr/share/lidar/asound-dsp.conf"
grep -q 'hw:Loopback,0,0' "$T/initramfs/usr/share/lidar/asound-dsp.conf"
grep -q 'period_size 384' "$T/initramfs/usr/share/lidar/asound-dsp.conf"
grep -q 'buffer_size 7680' "$T/initramfs/usr/share/lidar/asound-dsp.conf"

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
# No udev: name the GPT partitions ourselves (fw_env.config uses misc, the
# service config lives on the booted slot), so this comes first.
mkdir -p /dev/disk/by-partlabel /run /media/system
# The slot that booted: U-Boot puts it on the command line through the
# extlinux append line. Its filesystem holds /etc/lidar.conf (service
# switches); it stays mounted read-only and lidar-feature remounts it only
# to write.
slot=$(sed -n 's/.*\(^\| \)lidar\.slot=\([ab]\)\( .*\)\?$/\2/p' /proc/cmdline)
for i in $(seq 1 25); do
	for u in /sys/class/block/*/uevent; do
		n=$(sed -n 's/^PARTNAME=//p' "$u")
		[ -n "$n" ] && ln -sf "/dev/$(basename "$(dirname "$u")")" "/dev/disk/by-partlabel/$n"
	done
	[ -b /dev/disk/by-partlabel/misc ] && [ -b /dev/disk/by-partlabel/system_a ] &&
		[ -b "/dev/disk/by-partlabel/system_${slot:-a}" ] && break
	sleep 0.2
done
if [ -z "$slot" ] || ! mount -o ro -t ext4 "/dev/disk/by-partlabel/system_$slot" /media/system; then
	echo "lidar: booted slot not mounted (lidar.slot='$slot'): no /etc/lidar.conf, defaults apply" > /dev/kmsg
fi
# Light-ring animation until the last boot job touches /run/boot-done
if lidar-feature enabled bootanim; then
	lidar-bootanim /run/boot-done &
fi
# no udev here: load the sound stack by hand (codecs before the card)
for m in snd-soc-mt8516-afe snd-soc-tlv320adc3xxx snd-soc-tlv320aic32x4-i2c mt8516-amazon-lidar; do
	modprobe $m
done
# Base audio setup from the card's UCM profile (ucm2/, BootSequence): the
# verified microphone routing (PGA +35 dB), output driver gains as on
# Amazon's firmware, speaker volume -40 dB.
alsaucm -c hw:amazonlidar set _boot ''
# Speaker tuning (dsp): decided once, here, before any sound client starts.
# The owner's own Fire OS tuning files are staged from system_a (read-only);
# without them, or without snd-aloop, the speaker stays on the direct path.
# lidar-dsp takes the volume from lidar-volume, so it needs the volume service.
if lidar-feature enabled dsp && ! lidar-feature enabled volume; then
	echo "lidar: speaker tuning needs the volume service, speaker untuned" > /dev/kmsg
elif lidar-feature enabled dsp; then
	d=/run/lidar-dsp
	src=/mnt/fireos-dsp/system/vendor/etc/audio-algorithms
	mkdir -p $d /mnt/fireos-dsp
	if mount -o ro -t ext4 /dev/disk/by-partlabel/system_a /mnt/fireos-dsp; then
		cp $src/AFE.cfg $src/EQ_*.cfg $src/ParametricEQ.cfg $src/MBCL*.cfg $d/ 2>/dev/null
		umount /mnt/fireos-dsp
	fi
	if lidar-dsp -c $d -k 2>/dev/kmsg &&
	   modprobe snd-aloop pcm_substreams=1 timer_source=amazonlidar.0.0; then
		cp /usr/share/lidar/asound-dsp.conf /etc/asound.conf
		touch $d/active
		echo '::respawn:/usr/bin/lidar-dsp' >> /etc/inittab
	else
		echo "lidar: no Fire OS speaker tuning, speaker untuned" > /dev/kmsg
	fi
fi
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
# The clock (no RTC on this board): the last saved time now, NTP once the
# network is up
lidar-time restore
echo '::respawn:/usr/bin/lidar-time daemon' >> /etc/inittab
# System volume and LED ring: init owns the daemons and restarts them
if lidar-feature enabled volume; then
	echo '::respawn:/usr/bin/lidar-volume daemon' >> /etc/inittab
fi
if lidar-feature enabled ring && [ -e /sys/class/leds/ring1:red/brightness ]; then
	echo '::respawn:/usr/bin/lidar-ring daemon' >> /etc/inittab
fi
# Sound display on the ring: after the boot animation, as lidar-ring; it
# keeps off the ring while lidar-ring shows something
if lidar-feature enabled visualizer && [ -e /sys/class/leds/ring1:red/brightness ]; then
	echo "::respawn:/bin/sh -c 'while [ ! -e /run/boot-done ] || pidof lidar-bootanim >/dev/null; do sleep 0.5; done; exec /usr/bin/lidar-viz -s $(lidar-feature get visualizer style)'" \
		>> /etc/inittab
fi
# The EFR32 radio serves Zigbee and Thread at once (multi-PAN RCP): cpcd
# multiplexes it, zigbee runs zigbeed and its TCP bridge, thread runs
# otbr-agent; either or both.
if lidar-feature enabled zigbee || lidar-feature enabled thread; then
	echo '::respawn:/usr/bin/lidar-radio cpcd' >> /etc/inittab
fi
if lidar-feature enabled zigbee; then
	echo '::respawn:/usr/bin/lidar-radio zigbee' >> /etc/inittab
fi
if lidar-feature enabled thread; then
	echo '::respawn:/usr/bin/lidar-radio thread' >> /etc/inittab
	# usb: otbr-agent's REST API sits on a unix socket, and only
	# connections arriving on usb0 reach it (SO_BINDTODEVICE, as telnet)
	if [ "$(lidar-feature get thread listen)" != any ] && [ -e /sys/class/net/usb0 ]; then
		echo '::respawn:/usr/bin/lidar-listen usb0 8081 socat STDIO UNIX-CONNECT:/run/otbr-rest.sock' \
			>> /etc/inittab
	fi
fi
# Bluetooth speaker: the D-Bus system bus, bluetoothd, bluealsa and its
# player, and lidar-bluetooth's daemon (pairing, action button, volume);
# each waits for the one before, bluetoothd for the driver loaded below.
if lidar-feature enabled bluetooth; then
	mkdir -p /run/dbus
	dbus-uuidgen --ensure
	for s in 'dbus-daemon --system --nofork --nopidfile' 'lidar-bluetooth bluetoothd' \
		'lidar-bluetooth bluealsa' 'lidar-bluetooth aplay' 'lidar-bluetooth daemon'; do
		echo "::respawn:/usr/bin/$s" >> /etc/inittab
	done
fi
# Web interface: the player daemon, and httpd tied to usb0 unless the owner
# asks for every interface (as for telnet)
if lidar-feature enabled webui; then
	# the song library gets its own RAM disk: a hard limit, not a check
	mkdir -p /run/lidar-player
	mount -t tmpfs -o size="$(lidar-feature get webui storage_mb)m",mode=0755 tmpfs /run/lidar-player
	echo '::respawn:/usr/bin/lidar-player daemon' >> /etc/inittab
	www='busybox-extras httpd -h /usr/share/lidar/www'
	if [ "$(lidar-feature get webui listen)" = any ]; then
		echo "::respawn:$www -f -p 80" >> /etc/inittab
	elif [ -e /sys/class/net/usb0 ]; then
		echo "::respawn:/usr/bin/lidar-listen usb0 80 $www -i" >> /etc/inittab
	fi
fi
# Wi-Fi and Bluetooth share one chip (MT7668). For either, stage the owner's
# own Wi-Fi firmware and calibration from Fire OS (system_a, read-only; the
# ROM patch both functions share is the image's linux-firmware one) and load
# the WLAN driver: Bluetooth needs it first too, btmtksdio must not start
# while the WLAN driver downloads the patch. The regulatory domain is a
# cfg80211 user hint, so it can be set as soon as the driver has pulled
# cfg80211 in. Backgrounded so the shell never waits.
(
	wifi=; lidar-feature enabled wifi && wifi=1
	bt=; lidar-feature enabled bluetooth && bt=1
	if [ -n "$wifi$bt" ]; then
		dev=/dev/disk/by-partlabel/system_a
		fw=/mnt/fireos/system/vendor/firmware
		mkdir -p /mnt/fireos
		if [ -b "$dev" ] && mount -o ro -t ext4 "$dev" /mnt/fireos &&
		   cp $fw/WIFI_RAM_CODE*MT7668.bin $fw/EEPROM_MT7668.bi[n] \
		      $fw/TxPwrLimit_MT76x8.dat $fw/wifi.cfg /lib/firmware/ &&
		   modprobe wlan_mt76x8_sdio; then
			echo "lidar: Wi-Fi firmware staged, WLAN driver loaded" > /dev/kmsg
			cc=$(lidar-feature get wifi country)
			if [ -n "$cc" ]; then
				iw reg set "$cc" > /dev/kmsg 2>&1 &&
					echo "lidar: regulatory domain $cc requested" > /dev/kmsg
			fi
			if [ -n "$bt" ] && ! modprobe btmtksdio; then
				echo "lidar: Bluetooth driver failed to load" > /dev/kmsg
			fi
			# join the configured network, if any
			[ -z "$wifi" ] || lidar-wifi start
		else
			echo "lidar: Wi-Fi firmware not staged: no Wi-Fi, no Bluetooth" > /dev/kmsg
			touch /run/lidar-no-radio-firmware
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
	append console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000 loglevel=7 lidar.slot=${lidar_slot}
EOF

# The core services are on by default; this bring-up image also opts in to
# the extras it is used with. Edit with lidar-feature on the device.
mkdir -p "$T/fs/etc"
cat > "$T/fs/etc/lidar.conf" <<'EOF'
feature.usbaudio=1
feature.telnet=1
EOF
chmod 600 "$T/fs/etc/lidar.conf"

rm -f "$OUT/lidar-test-system.ext4"
mke2fs -q -t ext4 -L lidar-test -d "$T/fs" "$OUT/lidar-test-system.ext4" 256M
# Loose copies for pushing into a running system's /boot over the USB network
cp "$T/fs/boot/Image.gz" "$T/fs/boot/initramfs.gz" "$OUT/"
cp "$T/fs/boot/dtbs/mediatek/mt8516-amazon-lidar.dtb" "$OUT/"
ls -la "$OUT/lidar-test-system.ext4"
