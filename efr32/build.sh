#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds the OpenThread RCP for lidar's EFR32MG12 (see efr32/README.md).
#
#   podman build -t efr-build -f efr32/Containerfile efr32
#   podman run --rm -v efr-src:/efr -v "$PWD:/work" efr-build bash /work/efr32/build.sh
#
# Output: out/efr32/lidar-ot-rcp.gbl (for the Gecko bootloader) and the ELF.
# Sources (pinned) go to the efr-src volume:
#   /efr/gsdk                        Gecko SDK v4.4.3, the last with Series 1
#   /efr/gsdk/extension/ot-efr32     ot-efr32 before its move to Simplicity SDK
#   /efr/gsdk/extension/lidar        copy of efr32/extension
# slc 6 only loads extensions that are real directories inside the SDK, so
# ot-efr32 lives there and reaches the SDK through a symlink instead.
set -euo pipefail

GSDK_REV=d95099f4c05b3c40cece93d8a261dd471e642086 # v4.4.3
OT_EFR32_REV=089440d                               # 2024-07-29, last on Gecko SDK
SRC=/efr
S=$SRC/gsdk
R=$S/extension/ot-efr32
X=$S/extension/lidar
OUT=/work/out/efr32

fetch() {
	if [ ! -d "$S/.git" ]; then
		git clone -q --filter=blob:none --no-checkout https://github.com/SiliconLabs/gecko_sdk.git "$S"
	fi
	if [ "$(git -C "$S" rev-parse HEAD 2>/dev/null)" != "$GSDK_REV" ]; then
		git -C "$S" checkout -q "$GSDK_REV"
		git -C "$S" lfs install --local >/dev/null
		git -C "$S" lfs pull
	fi
	if [ ! -d "$R/.git" ]; then
		mkdir -p "$S/extension"
		git clone -q https://github.com/openthread/ot-efr32.git "$R"
	fi
	if [ "$(git -C "$R" rev-parse --short=7 HEAD)" != "$OT_EFR32_REV" ]; then
		git -C "$R" checkout -q -f "$OT_EFR32_REV"
	fi
	git -C "$R" submodule update -q --init --depth 1 openthread
	if [ ! -L "$R/third_party/silabs/gecko_sdk" ]; then
		rm -rf "$R/third_party/silabs/gecko_sdk"
		ln -s "$S" "$R/third_party/silabs/gecko_sdk"
	fi
	# ot-efr32 at this revision declares SDK 4.4.0 but pins 4.4.3; slc 6
	# only loads an extension whose SDK version matches exactly
	sed -i 's/^  version: 4\.4\.0$/  version: 4.4.3/' "$R/ot-efr32.slce"
}

generate() {
	local data=$SRC/build/slc.data gen=$SRC/build/gen
	rm -rf "$X" "$gen" && cp -r /work/efr32/extension "$X"
	mkdir -p "$gen"
	slc --daemon -v 1 signature trust --sdk "$S" -data "$data" >/dev/null
	slc --daemon -v 1 signature trust --sdk "$S" -extpath "$R" -data "$data" >/dev/null
	slc --daemon -v 1 signature trust --sdk "$S" -extpath "$X" -data "$data" >/dev/null
	slc --daemon -v 1 generate -data "$data" --sdk="$S" \
		--project-file="$X/projects/lidar-rcp-uart.slcp" --project-name=openthread-efr32-rcp-uart \
		--output-type=makefile --no-copy \
		--export-templates="$R/third_party/silabs/slc/exporter_templates/yaml" \
		--export-destination="$gen"
	mv "$gen/openthread-efr32-rcp-uart.Makefile" "$gen/slc_vars.yaml"
	rm -f "$gen/openthread-efr32-rcp-uart.project.mak"
	"$R/script/generate_cmake.py" "$gen/slc_vars.yaml" "$gen" >/dev/null
}

build() {
	local b=$SRC/build/rcp
	rm -rf "$b" && mkdir -p "$b"
	cmake -S "$R" -B "$b" -GNinja \
		-DCMAKE_TOOLCHAIN_FILE=src/efr32mg12/arm-none-eabi.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DEFR32_PLATFORM=efr32mg12 -DBOARD=lidar \
		-DOT_PLATFORM_LIB_DIR="$SRC/build/gen" \
		-DOT_FTD=OFF -DOT_MTD=OFF -DOT_RCP=ON \
		-DOT_APP_CLI=OFF -DOT_APP_NCP=OFF -DOT_APP_RCP=ON \
		-DOT_DIAGNOSTIC=ON -DOT_EXTERNAL_HEAP=ON -DOT_SLAAC=ON \
		-DOT_COMPILE_WARNING_AS_ERROR=ON > "$b/cmake.log" 2>&1 ||
		{ tail -20 "$b/cmake.log"; exit 1; }
	ninja -C "$b" ot-rcp > "$b/ninja.log" 2>&1 ||
		{ grep -E "error|FAILED" "$b/ninja.log" | head -20; exit 1; }
	mkdir -p "$OUT"
	cp "$b/bin/ot-rcp" "$OUT/lidar-ot-rcp.elf"
	arm-none-eabi-objcopy -O binary "$b/bin/ot-rcp" "$b/ot-rcp.bin"
	python3 /work/efr32/mkgbl.py "$b/ot-rcp.bin" "$OUT/lidar-ot-rcp.gbl"
	arm-none-eabi-size "$OUT/lidar-ot-rcp.elf"
}

fetch
generate
build
