#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds the multiprotocol (Zigbee + Thread) RCP for lidar's EFR32MG12 (see
# efr32/README.md).
#
#   podman build -t efr-build -f efr32/Containerfile efr32
#   podman run --rm -v efr-src:/efr -v "$PWD:/work" efr-build bash /work/efr32/build.sh
#
# Output: out/efr32/lidar-mp-rcp.gbl (for the Gecko bootloader) and the ELF.
# Sources (pinned) go to the efr-src volume:
#   /efr/gsdk                        Gecko SDK v4.4.3, the last with Series 1
#   /efr/gsdk/extension/lidar        copy of efr32/extension
# slc 6 only loads extensions that are real directories inside the SDK.
set -euo pipefail

GSDK_REV=d95099f4c05b3c40cece93d8a261dd471e642086 # v4.4.3
SRC=/efr
S=$SRC/gsdk
X=$S/extension/lidar
P=lidar-mp-rcp
B=$SRC/build/$P
OUT=/work/out/efr32

fetch() {
	if [ ! -d "$S/.git" ]; then
		git clone -q --filter=blob:none --no-checkout https://github.com/SiliconLabs/gecko_sdk.git "$S"
	fi
	if [ "$(git -C "$S" rev-parse HEAD 2>/dev/null)" != "$GSDK_REV" ]; then
		git -C "$S" checkout -q "$GSDK_REV"
	fi
	# every run: an interrupted pull would otherwise leave LFS pointer stubs
	# in place of the SDK's libraries for good (it skips what it has)
	git -C "$S" lfs install --local >/dev/null
	git -C "$S" lfs pull
}

generate() {
	local data=$SRC/build/slc.data
	rm -rf "$X" "$B" && cp -r /work/efr32/extension "$X"
	mkdir -p "$B"
	slc --daemon -v 1 signature trust --sdk "$S" -data "$data" >/dev/null
	slc --daemon -v 1 signature trust --sdk "$S" -extpath "$X" -data "$data" >/dev/null
	slc --daemon -v 1 generate -data "$data" --sdk="$S" \
		--project-file="$X/projects/$P.slcp" \
		--output-type=makefile --toolchain=gcc --no-copy \
		--export-destination="$B"
}

build() {
	make -C "$B" -f "$P.Makefile" -j"$(nproc)" release > "$B/make.log" 2>&1 ||
		{ grep -E "error|Error" "$B/make.log" | head -20; exit 1; }
	mkdir -p "$OUT"
	cp "$B/build/release/$P.out" "$OUT/$P.elf"
	arm-none-eabi-objcopy -O binary "$OUT/$P.elf" "$B/$P.bin"
	python3 /work/efr32/mkgbl.py "$B/$P.bin" "$OUT/$P.gbl"
	arm-none-eabi-size "$OUT/$P.elf"
}

fetch
generate
build
