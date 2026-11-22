#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/lidar-wakeword.cc and fetch its "Hey Jarvis" model.
#
# Runs inside the lidar-build container:
#   mk-wakeword.sh <out-dir> [alpine-sysroot]
# With an Alpine sysroot (what mk-testimg.sh passes) the binary is for
# aarch64 musl, built like the other tools there; without, for the build
# host, to try the detector on a PC:
#   sox hey.wav -t raw -r 16000 -e signed -b 16 -c 1 - | out/lidar-wakeword -m out/hey_jarvis.tflite -v
#
# <out-dir> receives lidar-wakeword, hey_jarvis.tflite, hey_jarvis.json and
# licenses/, the licence texts and notices that have to accompany the binary
# and the model.
#
# Nothing of this is in the repository. The sources are fetched once into
# /src, pinned by commit (git) or by SHA-256 (archives, model):
#   tflite-micro   TensorFlow Lite Micro, the interpreter (Apache-2.0)
#   tensorflow     only lite/experimental/microfrontend/lib, the feature
#                  frontend microWakeWord models are trained with
#                  (Apache-2.0); tflite-micro no longer carries it
#   flatbuffers    headers, the release tflite-micro's generated schema
#                  asserts (Apache-2.0)
#   kissfft        the frontend's FFT, as 16-bit fixed point (BSD-3-Clause)
#   hey_jarvis     microWakeWord v2 model and manifest from ESPHome's model
#                  collection (Apache-2.0, weights included, per its README
#                  and NOTICE)
set -euo pipefail
OUT=${1:?usage: mk-wakeword.sh <out-dir> [alpine-sysroot]}
SYSROOT=${2:-}

TFLM_COMMIT=38a8f0f87302817ddfa99e6a995c7b71ef7118fe
TFLM=/src/tflite-micro
if [ "$(cat $TFLM/.complete 2>/dev/null)" != $TFLM_COMMIT ]; then
	rm -rf $TFLM
	git init -q $TFLM
	git -C $TFLM remote add origin https://github.com/tensorflow/tflite-micro.git
	git -C $TFLM fetch -q --depth 1 origin $TFLM_COMMIT
	git -C $TFLM -c advice.detachedHead=false checkout -q FETCH_HEAD
	echo $TFLM_COMMIT > $TFLM/.complete
fi
TF_TAG=v2.21.0
TF_COMMIT=a481b10260dfdf833a1b16007eead49c1d7febf3
TF=/src/tensorflow-microfrontend
FE=tensorflow/lite/experimental/microfrontend/lib
if [ "$(cat $TF/.complete 2>/dev/null)" != $TF_COMMIT ]; then
	rm -rf $TF
	git init -q $TF
	git -C $TF remote add origin https://github.com/tensorflow/tensorflow.git
	git -C $TF sparse-checkout set --no-cone /$FE/
	git -C $TF fetch -q --depth 1 --filter=blob:none origin refs/tags/$TF_TAG
	git -C $TF -c advice.detachedHead=false checkout -q FETCH_HEAD
	[ "$(git -C $TF rev-parse HEAD)" = $TF_COMMIT ] ||
		{ echo "tensorflow $TF_TAG is no longer $TF_COMMIT" >&2; exit 1; }
	echo $TF_COMMIT > $TF/.complete
fi
# fetch_archive <dir> <url> <sha256> [tar member pattern...]
fetch_archive() {
	local dir=$1 url=$2 sum=$3
	shift 3
	[ "$(cat "$dir/.complete" 2>/dev/null)" = "$sum" ] && return
	rm -rf "$dir" "$dir.tar.gz"
	curl -fsSL "$url" -o "$dir.tar.gz"
	echo "$sum  $dir.tar.gz" | sha256sum -c --quiet
	mkdir "$dir"
	tar -xzf "$dir.tar.gz" -C "$dir" --strip-components=1 --wildcards "$@"
	rm "$dir.tar.gz"
	echo "$sum" > "$dir/.complete"
}
FB=/src/flatbuffers
fetch_archive $FB https://github.com/google/flatbuffers/archive/refs/tags/v25.9.23.tar.gz \
	9102253214dea6ae10c2ac966ea1ed2155d22202390b532d1dea64935c518ada '*/include' '*/LICENSE'
KISS=/src/kissfft
fetch_archive $KISS https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz \
	205a8f6a448ef12b091f8ac6a514b5091bb5f6b0b543431ed75f673116cf5cbf \
	'*/COPYING' '*/LICENSES/BSD-3-Clause' '*/_kiss_fft_guts.h' '*/kiss_fft.[ch]' '*/kiss_fft_log.h' \
	'*/kiss_fftr.[ch]'

MODELS_COMMIT=40ff33f57f8fc6ad71a75ef085abbf742495b225
MW=/src/micro-wake-word-models-$MODELS_COMMIT
if [ "$(cat $MW/.complete 2>/dev/null)" != $MODELS_COMMIT ]; then
	rm -rf $MW
	mkdir -p $MW
	MODELS=https://raw.githubusercontent.com/esphome/micro-wake-word-models/$MODELS_COMMIT
	curl -fsSL $MODELS/models/v2/hey_jarvis.tflite -o $MW/hey_jarvis.tflite
	curl -fsSL $MODELS/models/v2/hey_jarvis.json -o $MW/hey_jarvis.json
	curl -fsSL $MODELS/NOTICE -o $MW/NOTICE
	sha256sum -c --quiet <<EOF
21a7976add39ee24ec96c63d96b7aaa18e24d1d9824b963e451da8feb4b78b77  $MW/hey_jarvis.tflite
b153867d818675d8abcc9dace474afe7f83551ae0d5a9b1d71a98681320185af  $MW/hey_jarvis.json
bf6c416d88749af557faecbaf874861c6a808c5d45f4c28a2e25f9f613ce91ad  $MW/NOTICE
EOF
	echo $MODELS_COMMIT > $MW/.complete
fi
mkdir -p "$OUT/licenses"
install -m644 $MW/hey_jarvis.tflite $MW/hey_jarvis.json "$OUT/"
install -m644 $MW/NOTICE "$OUT/licenses/NOTICE.micro-wake-word-models"
install -m644 $TFLM/LICENSE "$OUT/licenses/Apache-2.0.txt"
install -m644 $KISS/COPYING "$OUT/licenses/COPYING.kissfft"
install -m644 $KISS/LICENSES/BSD-3-Clause "$OUT/licenses/BSD-3-Clause.txt"

CC=(clang)
CXX=(clang++)
CMAKE_TARGET=()
if [ -n "$SYSROOT" ]; then
	CC+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT")
	CXX+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT")
	# No CMAKE_SYSTEM_NAME: tflite-micro's CMakeLists takes a cross build
	# for a microcontroller (TF_LITE_MCU_DEBUG_LOG: no abort(), no clock).
	# ar and ranlib are the host's, not the sysroot's.
	CMAKE_TARGET=(-DCMAKE_CXX_COMPILER_TARGET=aarch64-alpine-linux-musl "-DCMAKE_SYSROOT=$SYSROOT"
		-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER)
fi
# TensorFlow Lite Micro as a static library with its reference kernels,
# without the signal ops (the microfrontend above is not among them). Kept
# between runs.
OB=/src/build/tflm-$TFLM_COMMIT-$(basename "${SYSROOT:-native}")
cmake -S $TFLM -B "$OB" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
	"${CMAKE_TARGET[@]}" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DTFLM_FLATBUFFERS_DIR=$FB -DTFLM_ENABLE_SIGNAL=OFF -DTFLM_BUILD_TESTS=OFF \
	-DTFLM_INSTALL=OFF -DTFLM_REMOVE_VIRTUAL_DELETE=OFF > "$OUT/tflm.log" 2>&1 &&
	cmake --build "$OB" >> "$OUT/tflm.log" 2>&1 || { tail -n 40 "$OUT/tflm.log" >&2; exit 1; }
rm "$OUT/tflm.log"

# The frontend and KISS FFT are C but for the frontend's FFT glue. The
# frontend computes its tables in floating point: -ffp-contract=off keeps
# them, and with them every feature, the same on arm64 as on x86-64, where
# the models are trained (fused multiply-add rounds differently). The
# interpreter is built without exceptions and RTTI, and so is what derives
# from its classes.
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
(cd "$W" && "${CC[@]}" -std=gnu11 -O2 -ffp-contract=off -DFIXED_POINT=16 -I$TF -I$KISS -c \
	$TF/$FE/{filterbank,filterbank_util,frontend,frontend_util,log_lut,log_scale,log_scale_util}.c \
	$TF/$FE/{noise_reduction,noise_reduction_util,pcan_gain_control,pcan_gain_control_util,window,window_util}.c \
	$KISS/kiss_fft.c $KISS/kiss_fftr.c &&
	"${CXX[@]}" -std=c++17 -O2 -ffp-contract=off -I$TF -I$KISS -c $TF/$FE/fft.cc $TF/$FE/fft_util.cc)
"${CXX[@]}" -fuse-ld=lld -std=c++17 -O2 -fno-exceptions -fno-rtti \
	-Wall -Wextra -Wno-nullability-completeness \
	-DFLATBUFFERS_LOCALE_INDEPENDENT=0 -DFLATBUFFERS_PREFER_PRINTF=1 \
	-isystem $TFLM -isystem $FB/include -isystem $TF \
	-s -o "$OUT/lidar-wakeword" "$(dirname "$0")/lidar-wakeword.cc" \
	"$W"/*.o "$OB/libtflm_micro.a" -lm
