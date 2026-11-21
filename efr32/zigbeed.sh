#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds zigbeed, the Zigbee stack that runs on the Echo next to the
# multiprotocol RCP (see efr32/README.md, "zigbeed").
#
#   podman build -t efr-build -f efr32/Containerfile efr32
#   podman run --rm -v efr-src:/efr -v "$PWD:/work" efr-build bash /work/efr32/zigbeed.sh
#
# Output: out/zigbeed/zigbeed, a static aarch64 program. The Zigbee stack
# itself only exists as Silicon Labs' prebuilt libraries (built against
# glibc), so this links everything statically with Debian's glibc cross
# toolchain; the result runs on the Echo's musl userland as it is.
# Sources (pinned) in the efr-src volume:
#   /efr/gsdk          Gecko SDK v4.4.3 (shared with build.sh)
#   /efr/cpc-daemon    cpc-daemon v4.4.6, for libcpc
set -euo pipefail

GSDK_REV=d95099f4c05b3c40cece93d8a261dd471e642086 # v4.4.3
CPC_REV=5e10b3e45ace73058327425a3fbedf06b4bcccbb # v4.4.6
SRC=/efr
S=$SRC/gsdk
C=$SRC/cpc-daemon
B=$SRC/build/zigbeed
OUT=/work/out/zigbeed
T=aarch64-linux-gnu

fetch() {
	if [ ! -d "$S/.git" ]; then
		git clone -q --filter=blob:none --no-checkout https://github.com/SiliconLabs/gecko_sdk.git "$S"
	fi
	if [ "$(git -C "$S" rev-parse HEAD 2>/dev/null)" != "$GSDK_REV" ]; then
		git -C "$S" checkout -q "$GSDK_REV"
	fi
	# the stack libraries are LFS objects; every run, as in build.sh
	git -C "$S" lfs install --local >/dev/null
	git -C "$S" lfs pull -I "protocol/zigbee/build/gcc/arm64v8/**"
	if [ ! -d "$C/.git" ]; then
		git clone -q https://github.com/SiliconLabs/cpc-daemon.git "$C"
	fi
	if [ "$(git -C "$C" rev-parse HEAD)" != "$CPC_REV" ]; then
		git -C "$C" checkout -q -f "$CPC_REV"
	fi
	# libcpc, cpcd and the RCP must speak the same CPC protocol
	local host rcp
	host=$(sed -n 's/^set(CPC_PROTOCOL_VERSION "\([0-9]*\)")$/\1/p' "$C/CMakeLists.txt")
	rcp=$(sed -n 's/^#define SLI_CPC_PROTOCOL_VERSION *(\([0-9]*\))$/\1/p' "$S/platform/service/cpc/inc/sli_cpc.h")
	if [ -z "$host" ] || [ "$host" != "$rcp" ]; then
		echo "CPC protocol version: cpc-daemon '$host', Gecko SDK '$rcp'" >&2
		exit 1
	fi
}

# libcpc.a from the objects of cpc-daemon's own (shared) library target, so
# that its build settings apply: socket folder /dev/shm, instance cpcd_0.
libcpc() {
	cmake -S "$C" -B "$B/cpc" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
		-DCMAKE_C_COMPILER=$T-gcc -DCMAKE_BUILD_TYPE=Release \
		-DENABLE_ENCRYPTION=FALSE > "$B/cpc.log" 2>&1 &&
		make -C "$B/cpc" cpc >> "$B/cpc.log" 2>&1 ||
		{ tail -20 "$B/cpc.log"; exit 1; }
	rm -f "$B/cpc/libcpc.a"
	find "$B/cpc/CMakeFiles/cpc.dir" -name '*.o' -exec $T-ar rcs "$B/cpc/libcpc.a" {} +
}

# The SDK's own zigbeed project; zigbee_arm64 selects the arm64v8 stack
# libraries.
generate() {
	local data=$B/slc.data
	rm -rf "$B/gen"
	slc --daemon -v 1 signature trust --sdk "$S" -data "$data" >/dev/null
	slc --daemon -v 1 generate -data "$data" --sdk="$S" \
		--project-file="$S/protocol/zigbee/app/zigbeed/zigbeed.slcp" \
		--with=linux_arch_64,zigbee_arm64 \
		--output-type=makefile --no-copy \
		--export-destination="$B/gen" > "$B/slc.log" 2>&1 ||
		{ tail -20 "$B/slc.log"; exit 1; }
}

# A reset from the host (ASH RST) restarts zigbeed through execvp. bellows
# (ZHA) waits 2.5 s for the answer; later it retries but keeps talking the
# wrong EZSP frame format. Patched copies make the restart quick:
# - app.c drops the second of sleep the SDK puts in before a restarted
#   zigbeed connects again (libcpc's sockets are close-on-exec anyway)
# - cpc_interface.cpp answers the radio's Spinel RESET itself, but OpenThread
#   then still blocks in one read for the (never coming) reply, 2 s; after
#   that fake reply the read no longer blocks
patch_sources() {
	local o=$S/protocol/openthread/platform-abstraction/posix p=$B/src m=$B/gen/zigbeed.project.mak
	mkdir -p "$p"
	cp "$S/protocol/zigbee/app/zigbeed/app.c" "$o/cpc_interface.cpp" "$p/"
	sed -i -e '/^  \/\/ Sleep added for successful restart of zigbeed when Host application resets$/d' \
		-e '/^  sleep(1);$/d' "$p/app.c"
	sed -i -e 's|^volatile sig_atomic_t CpcInterfaceImpl::sCpcResetReq = false;$|&\nstatic bool sResetAnswered = false;|' \
		-e 's|^    // Put CPC Reset call here$|    sResetAnswered = true;|' \
		-e 's|^    Read(aTimeoutUs);$|    Read(sResetAnswered ? 0 : aTimeoutUs);\n    sResetAnswered = false;|' \
		"$p/cpc_interface.cpp"
	sed -i -e "s|\$(SDK_PATH)/protocol/zigbee/app/zigbeed/app.c|$p/app.c|g" \
		-e "s|\$(SDK_PATH)/protocol/openthread/platform-abstraction/posix/cpc_interface.cpp|$p/cpc_interface.cpp|g" \
		"$m"
	if grep -q 'sleep(1)' "$p/app.c" || [ "$(grep -c sResetAnswered "$p/cpc_interface.cpp")" != 4 ] ||
	   ! grep -q "$p/app.c" "$m" || ! grep -q "$p/cpc_interface.cpp" "$m" ||
	   grep -q -E '\$\(SDK_PATH\)/protocol/(zigbee/app/zigbeed/app\.c|openthread/platform-abstraction/posix/cpc_interface\.cpp)' "$m"; then
		echo "zigbeed sources changed: the restart patch no longer applies" >&2
		exit 1
	fi
}

build() {
	# sl_cpc.h is not in the SDK; the generated makefile takes no extra
	# include path, the compiler does (also for the patched copies, whose
	# own headers stay in the SDK)
	CPATH=$C/lib:$S/protocol/zigbee/app/zigbeed make -C "$B/gen" -f zigbeed.Makefile -j"$(nproc)" release \
		CC=$T-gcc CXX=$T-g++ LD=$T-g++ AR=$T-ar \
		LD_FLAGS="-static -L$B/cpc" > "$B/make.log" 2>&1 ||
		{ grep -E -A5 "error|undefined" "$B/make.log" | head -40; exit 1; }
	# static glibc cannot load NSS or iconv modules on the Echo
	if grep -i warning "$B/make.log"; then
		exit 1
	fi
	mkdir -p "$OUT"
	$T-strip -o "$OUT/zigbeed" "$B/gen/build/release/zigbeed"
	file "$OUT/zigbeed"
	file "$OUT/zigbeed" | grep -q "ARM aarch64.*statically linked"
	stat -c '%s bytes' "$OUT/zigbeed"
}

mkdir -p "$B"
fetch
libcpc
generate
patch_sources
build
