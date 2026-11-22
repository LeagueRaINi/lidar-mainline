#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build lidar-tts (the assistant's voice) and lidar-say, with sherpa-onnx
# and espeak-ng, and the espeak-ng data lidar-tts phonemizes with.
#
# Runs inside the lidar-build container:
#   build.sh <out-dir> [alpine-sysroot]
# With an Alpine sysroot (what mk-testimg.sh passes; it needs
# onnxruntime-dev and alsa-lib-dev) the binaries are for aarch64 musl.
# Without, for the build host, to try them on a PC (it needs
# libasound2-dev); the libraries go into <out-dir>/lib, where lidar-tts
# looks for them:
#   out/lidar-tts -e out/espeak-ng-data -m <dir with tts/ and info> -I "Es ist sieben Uhr."
#
# <out-dir> receives lidar-tts, lidar-say, espeak-ng-data/ (for
# /usr/share/espeak-ng-data: phoneme tables, intonations and the English
# and German dictionaries and voices, about 1 MB) and licenses/, the
# licence texts of espeak-ng.
#
# sherpa-onnx comes from tools/lidar-stt/build.sh, which this runs into
# <out-dir>/stt: the same pinned sources and the same cached build in /src,
# so the image carries one libsherpa-onnx-c-api.so for both.
# espeak-ng (GPL-3.0-or-later; ucd-tools, GPL-3.0-or-later, with Unicode
# data under the Unicode licence; parts under BSD-2-Clause and Apache-2.0)
# is fetched once into /src, pinned at the commit Piper 1.8 (piper1-gpl)
# builds with, so the phonemes are those the Piper voices expect. It is
# linked statically, without sound output, sonic, klatt, speech-player,
# mbrola or threads: lidar-tts only phonemizes with it. Its data is compiled
# by a build for the build host (the data is the same on both).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh <out-dir> [alpine-sysroot]}
SYSROOT=${2:-}

bash "$HERE/../lidar-stt/build.sh" "$OUT/stt" ${SYSROOT:+"$SYSROOT"}
# c-api.h from the sources lidar-stt/build.sh fetched
SHERPA=/src/sherpa-onnx-$(sed -n 's/^SHERPA_COMMIT=//p' "$HERE/../lidar-stt/build.sh")
[ -e "$SHERPA/sherpa-onnx/c-api/c-api.h" ] || { echo "build.sh: no sherpa-onnx sources in $SHERPA" >&2; exit 1; }

ESPEAK_COMMIT=724808c5a83f9ef95fdd0db886ba7ba537ff224a
ESPEAK=/src/espeak-ng-$ESPEAK_COMMIT
if [ "$(cat $ESPEAK/.complete 2>/dev/null)" != $ESPEAK_COMMIT ]; then
	rm -rf $ESPEAK
	git init -q $ESPEAK
	git -C $ESPEAK remote add origin https://github.com/espeak-ng/espeak-ng.git
	git -C $ESPEAK fetch -q --depth 1 origin $ESPEAK_COMMIT
	git -C $ESPEAK -c advice.detachedHead=false checkout -q FETCH_HEAD
	[ "$(git -C $ESPEAK rev-parse HEAD)" = $ESPEAK_COMMIT ] ||
		{ echo "espeak-ng: fetched $(git -C $ESPEAK rev-parse HEAD), not $ESPEAK_COMMIT" >&2; exit 1; }
	echo $ESPEAK_COMMIT > $ESPEAK/.complete
fi
# Only the phonemizer. SONIC_*: espeak-ng's cmake fetches sonic unless it
# finds one; it is not used.
ESPEAK_CMAKE=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=OFF
	-DUSE_ASYNC=OFF -DUSE_MBROLA=OFF -DUSE_LIBSONIC=OFF -DUSE_LIBPCAUDIO=OFF -DUSE_KLATT=OFF
	-DUSE_SPEECHPLAYER=OFF -DENABLE_TESTS=OFF -DSONIC_LIB=unused -DSONIC_INC=unused)
DICTS="de en"
EN=/src/build/espeak-ng-$ESPEAK_COMMIT-native
cmake -S $ESPEAK -B $EN "${ESPEAK_CMAKE[@]}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	> "$OUT/espeak.log" 2>&1 &&
	ninja -C $EN espeak-ng espeak-ng-bin $(for d in $DICTS; do echo espeak-ng-data/${d}_dict; done) \
		>> "$OUT/espeak.log" 2>&1 || { tail -n 40 "$OUT/espeak.log" >&2; exit 1; }

CC=(clang)
CXX=(clang++)
if [ -n "$SYSROOT" ]; then
	[ -e "$SYSROOT/usr/include/alsa/asoundlib.h" ] || { echo "build.sh: $SYSROOT lacks alsa-lib-dev" >&2; exit 1; }
	CPU=-mcpu=cortex-a35
	T=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld $CPU)
	CC+=("${T[@]}")
	CXX+=("${T[@]}")
	EB=/src/build/espeak-ng-$ESPEAK_COMMIT-$(basename "$SYSROOT")
	mkdir -p $EB
	cat > $EB/toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_C_FLAGS_INIT $CPU)
set(CMAKE_CXX_FLAGS_INIT $CPU)
set(CMAKE_EXE_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
	cmake -S $ESPEAK -B $EB "${ESPEAK_CMAKE[@]}" -DCMAKE_TOOLCHAIN_FILE=$EB/toolchain.cmake \
		-DCOMPILE_INTONATIONS=OFF >> "$OUT/espeak.log" 2>&1 &&
		ninja -C $EB espeak-ng >> "$OUT/espeak.log" 2>&1 || { tail -n 40 "$OUT/espeak.log" >&2; exit 1; }
	LIB=(-L"$OUT/stt")
else
	[ -e /usr/include/alsa/asoundlib.h ] || { echo "build.sh: needs libasound2-dev (apt-get install)" >&2; exit 1; }
	EB=$EN
	mkdir -p "$OUT/lib"
	cp -a "$OUT"/stt/lib/* "$OUT/lib/"
	LIB=(-L"$OUT/lib" -Wl,-rpath,'$ORIGIN/lib')
fi
rm "$OUT/espeak.log"

# the data: what phonemizing English and German needs
rm -rf "$OUT/espeak-ng-data"
mkdir -p "$OUT/espeak-ng-data/lang" "$OUT/licenses"
for f in phondata phonindex phontab intonations $(for d in $DICTS; do echo ${d}_dict; done); do
	install -m644 $EN/espeak-ng-data/$f "$OUT/espeak-ng-data/"
done
cp -r $EN/espeak-ng-data/lang/gmw "$OUT/espeak-ng-data/lang/"
for f in COPYING COPYING.APACHE COPYING.BSD2 COPYING.UCD; do
	install -m644 $ESPEAK/$f "$OUT/licenses/$f.espeak-ng"
done

FLAGS=(-O2 -Wall -Wextra -Wno-nullability-completeness -s)
"${CC[@]}" -std=gnu11 "${FLAGS[@]}" -o "$OUT/lidar-say" "$HERE/lidar-say.c"
"${CXX[@]}" -std=c++17 "${FLAGS[@]}" -ffp-contract=fast -pthread -I"$SHERPA" -I$ESPEAK/src/include \
	-o "$OUT/lidar-tts" "$HERE/lidar-tts.cc" "${LIB[@]}" -lsherpa-onnx-c-api \
	$EB/src/libespeak-ng/libespeak-ng.a $EB/src/ucd-tools/libucd.a -lasound
