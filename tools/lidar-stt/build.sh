#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build lidar-stt (speech recognition) and lidar-tee, with sherpa-onnx.
#
# Runs inside the lidar-build container:
#   build.sh <out-dir> [alpine-sysroot]
# With an Alpine sysroot (what mk-testimg.sh passes; it needs onnxruntime-dev)
# the binaries are for aarch64 musl, built like the other tools there, and
# sherpa-onnx is built against Alpine's onnxruntime, which the image carries
# as a package. Without, for the build host, to try them on a PC; sherpa-onnx
# then fetches its own onnxruntime build (pinned by its cmake/ files), and
# the libraries go into <out-dir>/lib, where the binary looks for them:
#   sox command.wav -t raw -r 16000 -e signed -b 16 -c 1 - |
#     out/lidar-stt -m <dir with stt/> -W 0 -o - -v
#
# <out-dir> receives lidar-stt, lidar-tee, libsherpa-onnx-c-api.so (the
# recognizer, built with text-to-speech too, for /usr/lib) and licenses/,
# the licence texts of sherpa-onnx and of what it compiles in.
#
# sherpa-onnx is fetched once into /src, pinned by commit (Apache-2.0). Its
# cmake files fetch the libraries it compiles in, each pinned by SHA-256:
# kaldi-native-fbank, kaldi-decoder, kaldifst, kaldi-hmm-gmm,
# kaldi-native-io, simple-sentencepiece (Apache-2.0), OpenFst (Apache-2.0),
# Eigen (MPL-2.0, headers), nlohmann/json, utf8proc (MIT), KISS FFT
# (BSD-3-Clause).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh <out-dir> [alpine-sysroot]}
SYSROOT=${2:-}

SHERPA_TAG=v2.0.2
SHERPA_COMMIT=5d2bd37de78b223a2e8e00e25483d12c5b164299
SHERPA=/src/sherpa-onnx-$SHERPA_COMMIT
if [ "$(cat $SHERPA/.complete 2>/dev/null)" != $SHERPA_COMMIT ]; then
	rm -rf $SHERPA
	git init -q $SHERPA
	git -C $SHERPA remote add origin https://github.com/k2-fsa/sherpa-onnx.git
	git -C $SHERPA fetch -q --depth 1 origin refs/tags/$SHERPA_TAG
	git -C $SHERPA -c advice.detachedHead=false checkout -q FETCH_HEAD
	[ "$(git -C $SHERPA rev-parse HEAD)" = $SHERPA_COMMIT ] ||
		{ echo "sherpa-onnx $SHERPA_TAG is no longer $SHERPA_COMMIT" >&2; exit 1; }
	echo $SHERPA_COMMIT > $SHERPA/.complete
fi

CC=(clang)
CXX=(clang++)
CMAKE_TARGET=()
OB=/src/build/sherpa-onnx-$SHERPA_COMMIT-$(basename "${SYSROOT:-native}")
mkdir -p "$OB" "$OUT/licenses"
if [ -n "$SYSROOT" ]; then
	[ -e "$SYSROOT/usr/include/onnxruntime/onnxruntime_c_api.h" ] ||
		{ echo "build.sh: $SYSROOT lacks onnxruntime-dev" >&2; exit 1; }
	CPU=-mcpu=cortex-a35
	CC+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld $CPU)
	CXX+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld $CPU)
	cat > "$OB/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET aarch64-alpine-linux-musl)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -s")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -s")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
	CMAKE_TARGET=(-DCMAKE_TOOLCHAIN_FILE="$OB/toolchain.cmake" "-DCMAKE_C_FLAGS=$CPU"
		"-DCMAKE_CXX_FLAGS=$CPU -Wno-nullability-completeness -include cstdint"
		-DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=ON)
	export SHERPA_ONNXRUNTIME_INCLUDE_DIR=$SYSROOT/usr/include/onnxruntime
	export SHERPA_ONNXRUNTIME_LIB_DIR=$SYSROOT/usr/lib
else
	CMAKE_TARGET=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-include cstdint"
		-DCMAKE_SHARED_LINKER_FLAGS=-s -DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=OFF)
fi
# The shared C API library, with sherpa-onnx's core and the libraries above
# inside; onnxruntime stays a library of its own. Kept between runs.
cmake -S $SHERPA -B "$OB" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_TARGET[@]}" \
	-DBUILD_SHARED_LIBS=ON -DSHERPA_ONNX_ENABLE_C_API=ON -DSHERPA_ONNX_ENABLE_BINARY=OFF \
	-DSHERPA_ONNX_ENABLE_TTS=ON -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
	-DSHERPA_ONNX_ENABLE_PYTHON=OFF -DSHERPA_ONNX_ENABLE_TESTS=OFF -DSHERPA_ONNX_ENABLE_JNI=OFF \
	-DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF -DSHERPA_ONNX_ENABLE_GPU=OFF \
	-DCMAKE_INSTALL_PREFIX="$OB/install" > "$OUT/sherpa.log" 2>&1 &&
	ninja -C "$OB" install >> "$OUT/sherpa.log" 2>&1 || { tail -n 40 "$OUT/sherpa.log" >&2; exit 1; }
rm "$OUT/sherpa.log"
I=$OB/install

install -m644 $SHERPA/LICENSE "$OUT/licenses/LICENSE.sherpa-onnx"
# each library's licence files; a LICENSES/ directory only where there is no
# LICENSE (nlohmann/json's also covers its tooling, which is not built)
for d in "$OB"/_deps/*-src; do
	name=$(basename "$d" -src)
	files=("$d"/LICEN[CS]E* "$d"/COPYING* "$d"/NOTICE*)
	compgen -G "$d/LICEN[CS]E*" > /dev/null || files+=("$d"/LICENSES/*)
	for f in "${files[@]}"; do
		if [ -f "$f" ]; then
			install -m644 "$f" "$OUT/licenses/$(basename "$f").$name"
		fi
	done
done

FLAGS=(-O2 -Wall -Wextra -Wno-nullability-completeness -s)
"${CC[@]}" -std=gnu11 "${FLAGS[@]}" -o "$OUT/lidar-tee" "$HERE/lidar-tee.c"
if [ -n "$SYSROOT" ]; then
	install -m644 $I/lib/libsherpa-onnx-c-api.so "$OUT/"
	"${CXX[@]}" -std=c++17 "${FLAGS[@]}" -I$I/include -o "$OUT/lidar-stt" "$HERE/lidar-stt.cc" \
		-L$I/lib -lsherpa-onnx-c-api
else
	mkdir -p "$OUT/lib"
	cp -a $I/lib/libsherpa-onnx-c-api.so $I/lib/libonnxruntime.so* "$OUT/lib/"
	"${CXX[@]}" -std=c++17 "${FLAGS[@]}" -I$I/include -o "$OUT/lidar-stt" "$HERE/lidar-stt.cc" \
		-L$I/lib -lsherpa-onnx-c-api -Wl,-rpath,'$ORIGIN/lib'
fi
