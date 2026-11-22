#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build lidar-claude. Runs inside the lidar-build container:
#   build.sh <out-dir> [alpine-sysroot]   build <out-dir>/lidar-claude
# With an Alpine sysroot (what mk-testimg.sh passes; it needs openssl-dev
# and cjson-dev) the binary is for aarch64 musl, built like the other tools
# there; without, for the build host (libssl-dev and libcjson-dev).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh <out-dir> [alpine-sysroot]}
SYSROOT=${2:-}

CXX=(clang++)
if [ -n "$SYSROOT" ]; then
	CXX+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld
		-Wno-nullability-completeness)
fi
mkdir -p "$OUT"
"${CXX[@]}" -std=c++17 -O2 -Wall -Wextra -pthread -s -o "$OUT/lidar-claude" \
	"$HERE/lidar-claude.cc" -lcjson -lssl -lcrypto
