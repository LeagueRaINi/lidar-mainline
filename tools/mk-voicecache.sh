#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# The phrase cache of a voice, made on the build host, so the Echo need not
# spend 15-20 minutes on it: lidar-tts -B renders every phrase and number
# word of lidar-assistant -P and every fixed sentence of lidar-claude -P with
# the voice exactly as lidar-models installs it on the Echo, and the files
# go into a tar that lidar-models install unpacks into <models>/<voice>/cache.
#
# Runs inside the lidar-build container:
#   mk-voicecache.sh <language> <voice> <out.tar>
#   e.g. mk-voicecache.sh de thorsten /work/out/lidar-voicecache-thorsten.tar
#
# The voice comes from lidar-models.sh itself (its pins and checks, the
# metadata and tokens it adds), installed into /src/voicecache/models; that
# also fetches the language's recognizer, as lidar-models installs both
# (once per language: the next voice of a language keeps it). lidar-tts,
# lidar-assistant and lidar-claude are built for the host with their
# build.sh (the host needs libasound2-dev, libcjson-dev, wget and bzip2).
#
# The tar: ustar, the cache files (no directory) sorted by name, owner root,
# mode 644, mtime 0. Its layout is reproducible, the audio is not: the
# voice's noise is random.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
L=${1:?usage: mk-voicecache.sh <language> <voice> <out.tar>}
VOICE=${2:?usage: mk-voicecache.sh <language> <voice> <out.tar>}
TAR=${3:?usage: mk-voicecache.sh <language> <voice> <out.tar>}
W=/src/voicecache
B=$W/build

for t in wget bzip2; do
	command -v $t > /dev/null || { echo "mk-voicecache: needs $t" >&2; exit 1; }
done
t0=$(date +%s)
mkdir -p "$B"
bash "$HERE/lidar-tts/build.sh" "$B/tts"
bash "$HERE/lidar-assistant/build.sh" "$B/assistant"
bash "$HERE/lidar-claude/build.sh" "$B/claude"
# lidar-assistant reads its language files from the repository
cat > "$B/lidar-assistant" <<EOF
#!/bin/sh
exec "$B/assistant/lidar-assistant" -L "$HERE/lidar-assistant/lang" "\$@"
EOF
chmod +x "$B/lidar-assistant"
t1=$(date +%s)

# -R: /src is no flash, and that is fine here
sh "$HERE/lidar-models.sh" -d "$W/models" -p "$W/progress" -R install "$L" -v "$VOICE"
[ "$(readlink "$W/models/current")" = "$L-$VOICE" ] ||
	{ echo "mk-voicecache: lidar-models did not install $L $VOICE" >&2; exit 1; }
t2=$(date +%s)

C=$W/cache-$L-$VOICE
rm -rf "$C"
threads=$(nproc)
[ "$threads" -le 64 ] || threads=64
"$B/tts/lidar-tts" -B "$C" -m "$W/models/current" -e "$B/tts/espeak-ng-data" \
	-a "$B/lidar-assistant" -A "$B/claude/lidar-claude" -t "$threads"
t3=$(date +%s)

mkdir -p "$(dirname "$TAR")"
(cd "$C" && find . -maxdepth 1 -type f -printf '%P\n' | LC_ALL=C sort) > "$W/files"
tar --format=ustar --owner=0 --group=0 --numeric-owner --mode=644 --mtime=@0 \
	-C "$C" -cf "$TAR.tmp" -T "$W/files"
mv "$TAR.tmp" "$TAR"
echo "mk-voicecache: $TAR: $(wc -l < "$W/files") files, $(($(stat -c %s "$TAR") >> 10)) kB;" \
	"build $((t1 - t0)) s, voice $((t2 - t1)) s, rendering $((t3 - t2)) s"
