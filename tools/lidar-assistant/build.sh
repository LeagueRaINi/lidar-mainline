#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build lidar-assistant, and test it on the build host.
#
# Runs inside the lidar-build container:
#   build.sh <out-dir> [alpine-sysroot]   build <out-dir>/lidar-assistant
#   build.sh test <out-dir>               build for the host and run the tests
#   build.sh expect <out-dir>             build for the host and rewrite what
#                                         the tests expect of the sessions
#                                         (tests/session-*.out) and the phrase
#                                         lists (phrases-*.txt): read the diff
# With an Alpine sysroot (what mk-testimg.sh passes) the binary is for
# aarch64 musl, built like the other tools there; without, for the build
# host. It needs its language files next to it at run time: lang/*.txt,
# /usr/share/lidar/assistant on the Echo.
#
# The tests:
#   tests/utterances.txt  what the grammar makes of utterances, both languages
#   tests/session-*.txt   whole sessions in test mode (-t): actions, answers,
#                         timers and alarms on a clock the session moves;
#                         tests/session-*.out is the output they have to give
#   phrases-*.txt         every phrase and number word the answers are made
#                         of, for the text-to-speech cache; has to match -P
#   and the daemon for a few seconds: a timer set through the FIFO rings
#   into a stand-in for aplay until "!stop"
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=build
case ${1:-} in test | expect) MODE=$1; shift ;; esac
OUT=${1:?usage: build.sh [test|expect] <out-dir> [alpine-sysroot]}
SYSROOT=${2:-}

CXX=(clang++)
if [ -n "$SYSROOT" ]; then
	CXX+=(--target=aarch64-alpine-linux-musl --sysroot="$SYSROOT" -fuse-ld=lld
		-Wno-nullability-completeness)
fi
mkdir -p "$OUT"
"${CXX[@]}" -std=c++17 -O2 -Wall -Wextra -s -o "$OUT/lidar-assistant" \
	"$HERE/lidar-assistant.cc" "$HERE/grammar.cc" "$HERE/speech.cc"
[ $MODE = build ] && exit 0

BIN="$OUT/lidar-assistant"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Sessions run in a fixed time zone, with a volume state to read
session() { # language
	rm -rf "$TMP/run" "$TMP/data"
	mkdir -p "$TMP/run"
	echo "60 0 127" > "$TMP/run/lidar-volume.state"
	TZ=CET-1CEST,M3.5.0,M10.5.0/3 "$BIN" -t -l "$1" -L "$HERE/lang" -d "$TMP/data" -r "$TMP/run" \
		< "$HERE/tests/session-$1.txt"
}

for l in en de; do
	if [ $MODE = expect ]; then
		session $l > "$HERE/tests/session-$l.out"
		"$BIN" -P -l $l -L "$HERE/lang" > "$HERE/phrases-$l.txt"
		continue
	fi
	# the grammar: what it gives next to what it has to give
	awk -F ' [|] ' -v l=$l '$1 == l { print $2 }' "$HERE/tests/utterances.txt" > "$TMP/said"
	awk -F ' [|] ' -v l=$l '$1 == l { print $3 }' "$HERE/tests/utterances.txt" > "$TMP/want"
	"$BIN" -t -n -l $l -L "$HERE/lang" < "$TMP/said" | sed 's/^action //' > "$TMP/got"
	paste -d '|' "$TMP/said" "$TMP/want" "$TMP/got" | awk -F '|' -v l=$l '
		$2 != $3 { printf "FAIL %s: \"%s\" gives \"%s\", not \"%s\"\n", l, $1, $3, $2; bad++ }
		END { printf "%s: %d utterances, %d failed\n", l, NR, bad; exit bad != 0 || NR == 0 }' ||
		fail=1
	session $l | diff -u "$HERE/tests/session-$l.out" - || { echo "FAIL session-$l"; fail=1; }
	"$BIN" -P -l $l -L "$HERE/lang" | diff -u "$HERE/phrases-$l.txt" - ||
		{ echo "FAIL phrases-$l.txt"; fail=1; }
done
[ $MODE = expect ] && exit 0

# the actions are valid JSON, the chime rises and does not clip
"$BIN" -S | python3 -c 'import json, sys; print("actions:", len(json.load(sys.stdin)))' || fail=1
"$BIN" -T 40 | python3 -c '
import array, sys
a = array.array("h", sys.stdin.buffer.read())
peak = lambda s: max(abs(x) for x in a[s * 96000:(s + 1) * 96000])
p = [peak(s) for s in (0, 10, 20, 30, 39)]
print("chime: peak per second at 0, 10, 20, 30, 39 s:", p)
assert len(a) == 40 * 96000 and p[0] < p[1] < p[2] < p[3] and p[3] == p[4] and 300 < p[0] < 700 and 12000 < p[4] < 16500
' || fail=1

# bytes that are no UTF-8 are skipped, whatever follows 0xC3
[ "$(printf 'stop \303\377\303\300\n' | "$BIN" -t -n -L "$HERE/lang")" = "action stop" ] ||
	{ echo "FAIL invalid UTF-8"; fail=1; }

# The daemon: FIFO (a regular file is in its place), state file, ringing.
# The speaker is "muted at 8 %", so it has to unmute and turn up for the
# ringing, and go back after it; lidar-volume is a stand-in that keeps the
# state file as the real one does. A line that is too long is dropped whole,
# a failing lidar-bluetooth is said, and the player ends with the daemon.
mkdir -p "$TMP/bin" "$TMP/live"
printf '#!/bin/sh\ncat > "$LOG/chime.raw"\n' > "$TMP/bin/aplay"
printf '#!/bin/sh\nexit 1\n' > "$TMP/bin/lidar-bluetooth"
cat > "$TMP/bin/lidar-volume" <<'EOF'
#!/bin/sh
echo "$*" >> "$LOG/volume"
read -r l m v < "$LOG/live/lidar-volume.state"
case $1 in set) l=$(( ($2 * v + 50) / 100 )) ;; mute) m=1 ;; unmute) m=0 ;; esac
echo "$l $m $v" > "$LOG/live/lidar-volume.state"
EOF
chmod +x "$TMP/bin/aplay" "$TMP/bin/lidar-bluetooth" "$TMP/bin/lidar-volume"
echo "10 1 127" > "$TMP/live/lidar-volume.state"
: > "$TMP/live/lidar-time.synced"
echo "written while the daemon was down" > "$TMP/live/lidar-assistant.fifo"
LOG=$TMP PATH=$TMP/bin:$PATH "$BIN" -L "$HERE/lang" -d "$TMP/live" -r "$TMP/live" -b none \
	> "$TMP/live/out" &
daemon=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -p "$TMP/live/lidar-assistant.fifo" ] && break; sleep 0.2; done
{ head -c 3000 /dev/zero | tr '\0' x; echo " stop"; } > "$TMP/live/lidar-assistant.fifo"
echo "pause" > "$TMP/live/lidar-assistant.fifo"
echo "set a timer for one second" > "$TMP/live/lidar-assistant.fifo"
sleep 2.5
rang=$(cat "$TMP/live/lidar-assistant.json")
echo '!stop' > "$TMP/live/lidar-assistant.fifo"
sleep 0.5
quiet=$(cat "$TMP/live/lidar-assistant.json")
echo "set a timer for one second" > "$TMP/live/lidar-assistant.fifo"
sleep 2.5
kill -9 $daemon
wait $daemon 2>/dev/null || true
sleep 0.5
if [[ $rang == *'"ringing":[{"kind":"timer","name":""}]'* && $quiet == *'"ringing":[],"timers":[]'* ]] &&
	[ "$(tr '\n' ';' < "$TMP/volume")" = "unmute;set 30;set 8;mute;unmute;set 30;" ] &&
	[ "$(wc -c < "$TMP/chime.raw")" -gt 96000 ] && ! pgrep -f "$TMP/bin/aplay" > /dev/null &&
	[ "$(grep -c '^action' "$TMP/live/out")" = 3 ] &&
	grep -q '^say en That did not work\.$' "$TMP/live/out" &&
	grep -q '^say en Timer set for one second\.$' "$TMP/live/out"; then
	echo "daemon: timer rang and stopped, player gone with the daemon"
else
	echo "FAIL daemon: $rang / $quiet / $(cat "$TMP/volume" "$TMP/live/out" 2>&1; pgrep -af aplay)"
	fail=1
fi
[ $fail = 0 ] && echo "all tests passed"
exit $fail
