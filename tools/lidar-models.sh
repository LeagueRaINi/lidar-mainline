#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Speech models of the voice assistant: speech recognition (sherpa-onnx
# streaming zipformer "Kroko") and a Piper voice for one language at a
# time, English or German. They are too large for the image (about 130 MB
# per language) and are downloaded on the device, every file pinned below
# by URL, size and sha256.
#
# Usage: lidar-models [-d <dir>] [-p <file>] status
#        lidar-models [-d <dir>] [-p <file>] install en|de [-v <voice>]
#        lidar-models [-d <dir>] remove en|de
#        lidar-models [-d <dir>] verify
#        lidar-models [-d <dir>] grow-fs
#   -d  where the models live (default /var/lib/lidar-models); it has to be
#       on flash, not in RAM: at boot "lidar-feature keep" binds it from the
#       booted slot
#   -p  progress file of install (default /run/lidar-models.progress)
#   -v  voice: en kristin; de thorsten (default) or eva_k (smaller, faster)
#   -R  install even if <dir> is in RAM (for tests)
#
# install downloads into <dir>/.new, checks every download and every
# resulting file against the pins and only then switches the symlink
# <dir>/current; the language installed before is removed after that. A
# failed or interrupted install leaves the installed language as it was.
# With the same language installed, only the voice is downloaded.
#
# What a language looks like (the paths the assistant loads):
#   current/info               lang, voice, espeak_voice, sample_rate,
#                              noise_scale, noise_w, length_scale (key=value)
#   current/stt/encoder.onnx, decoder.onnx, joiner.onnx, tokens.txt
#   current/tts/model.onnx     the Piper voice with the model metadata
#                              sherpa-onnx reads (comment=piper, language,
#                              voice, n_speakers, sample_rate, ...)
#   current/tts/tokens.txt     "<phoneme> <id>" per line, from the voice's
#                              phoneme_id_map
#   current/tts/clusters.txt   see below; empty for most voices
#   current/tts/model.onnx.json, MODEL_CARD   as published
#
# The voices are the files of rhasspy/piper-voices on Hugging Face at a
# pinned revision. sherpa-onnx needs two things they lack, which
# scripts/piper/add_meta_data.py of sherpa-onnx adds with Python and which
# are added here without it:
#   - the metadata: ONNX is a protobuf message and metadata_props a repeated
#     field of it, so the entries are appended to the file;
#   - tokens.txt. sherpa-onnx takes one code point per token. A voice
#     trained with Piper 1.5 or later (thorsten) has tokens of two code
#     points for diphthongs ("vowel_clusters" in model.onnx.json): each gets
#     a private-use code point (U+E000 and up) in tokens.txt, and
#     clusters.txt lists "<phonemes> <code point>" per line, both in UTF-8.
#     Whoever phonemizes has to replace each adjacent cluster by its code
#     point before handing the phonemes to sherpa-onnx, as Piper merges them.
# sherpa-onnx 2.x phonemizes nothing itself: the phonemes come from
# espeak-ng (voice espeak_voice), whose data is part of the image.
#
# Progress file: one line, replaced atomically, tab-separated:
#   <state> <percent> <language> <voice> <message>
# state is download, unpack, verify, done or error; percent counts the
# downloaded bytes (0-95) and is 100 at done. After done or error the line
# stays until the next install.
#
# grow-fs grows the ext4 filesystem that holds <dir> to the size of its
# partition (the image is smaller than the partition it is flashed to), with
# resize2fs on the mounted filesystem. It only ever grows, and does nothing
# unless the partition is larger than the filesystem.
DIR=/var/lib/lidar-models
PROGRESS=/run/lidar-models.progress
SLOT_LOCK=/run/lidar-slot.lock	# lidar-feature's lock for remounts of the slot
MARGIN=$((8 << 20))	# free space kept beyond the files themselves

HF=https://huggingface.co/rhasspy/piper-voices/resolve/e9c8a39444359388adaa94255becb37a5a2b8e51
GH=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models

# language, voice (the first of a language is its default), then what goes
# into the model metadata: language name, espeak-ng voice, sample rate; and
# the voice's inference settings for info: noise_scale, noise_w, length_scale
voices() {
	cat <<'EOF'
en kristin English en 22050 0.667 0.8 1
de thorsten German de 22050 0.5 0.333 1
de eva_k German de 16000 0.667 0.8 1
EOF
}

# Downloads: component, bytes, sha256, URL
sources() {
	cat <<EOF
stt-en 57267600 c8676e5ff9ac2a85296e53ee0fd4d5fb1db6770e7a7647166eeafe349ade6834 $GH/sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2
stt-de 57565698 9e27b783c20e67b0d0f13a258c1861fce199917c969d9176a438bee38df64962 $GH/sherpa-onnx-streaming-zipformer-de-kroko-2025-08-06.tar.bz2
tts-kristin 63531379 5849957f929cbf720c258f8458692d6103fff2f0e3d3b19c8259474bb06a18d4 $HF/en/en_US/kristin/medium/en_US-kristin-medium.onnx
tts-kristin 4968 5681426d4aead22195de70531eeeeddb46493cfaffc5764b2ea3db73428b651c $HF/en/en_US/kristin/medium/en_US-kristin-medium.onnx.json
tts-kristin 479 8c181b1d5f7d8152b914faab941fcbe6b8f0495e63e5189d6f27b8c68c8393fc $HF/en/en_US/kristin/medium/MODEL_CARD
tts-thorsten 63221984 fd270eb3d1ebf91d763f8a0f949b1b1bebdfefaea18c3aaf8577dd34536e064f $HF/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx
tts-thorsten 6455 e5bb7410dcbbcc0be6f4544f6bc66612b315ab5cb3099ec44bb46d860bc7cca9 $HF/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json
tts-thorsten 427 c638118fb06eb07d59e60270097c250cf2de81a037320a8a6306fec652ad13b0 $HF/de/de_DE/thorsten/medium/MODEL_CARD
tts-eva_k 20628813 e88cf290fbfb768bf111330d2e8a46e376b0d85e3423a28bfebbc863a260dad8 $HF/de/de_DE/eva_k/x_low/de_DE-eva_k-x_low.onnx
tts-eva_k 4158 ef14b3dcb279ab4b18422a7a132877bee7a148821bd91152fb7ae9c4b3d79625 $HF/de/de_DE/eva_k/x_low/de_DE-eva_k-x_low.onnx.json
tts-eva_k 246 187741f947241c18a8d8cdf773fcec75325172628442a866c2da44a5f23e590f $HF/de/de_DE/eva_k/x_low/MODEL_CARD
EOF
}

# Installed files: component, file, bytes, sha256. Of a Kroko archive only
# these are kept (it has no int8 variant; the rest is a test recording). The
# voices' model.onnx and tokens.txt are the results of add_metadata and
# gen_tokens below.
files() {
	cat <<'EOF'
stt-en encoder.onnx 70092599 d4881c57449d581e0770fd53fa66c2fdc6cd167d92ece7c715e603defc96d9d4
stt-en decoder.onnx 617488 455ba38466fce8d5a57e7db68a323b684079ca4d9e1dd93a740d9b2429aae3b1
stt-en joiner.onnx 336817 d406f616736350e2a7df3e39398b78eb2fc1a2ca6973a19d3853fa3227e25b52
stt-en tokens.txt 6310 396dbeb5f4858875690716084f54e90d339679d0ba3e6b5b584f3d7589254d2d
stt-en README.md 55 f42a5c6d54eb811dd0dd624cbee0e86205fc7bcb05507097f3278cf1b0b5c6af
stt-de encoder.onnx 70091557 6e83993d6967ec7a3498b055b7e85ace85b5d64d1b1e8773cb29a43a11f5edb5
stt-de decoder.onnx 617489 94a29592b403c53fa2231b478637da1ab4abcef7f5e46e432098416a4a3ed562
stt-de joiner.onnx 336817 28356bff070aea51ab1d725a3278e81d19f9300f860d3248a7014292264df15a
stt-de tokens.txt 5606 86e8370994ff2c01149ba8c4f8709aa93cdc18914b27a717e291e96faf39a6eb
stt-de README.md 55 f42a5c6d54eb811dd0dd624cbee0e86205fc7bcb05507097f3278cf1b0b5c6af
tts-kristin model.onnx 63531536 b2605c9a9fc6ca04786b31b56a3a794129b9602433c986269f6423458810b1eb
tts-kristin tokens.txt 940 ef3a7e4a8d1af0c9d4dc45aaae1a6242ebe24a7ed6f3d025a49eb29682784c6d
tts-kristin clusters.txt 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
tts-kristin model.onnx.json 4968 5681426d4aead22195de70531eeeeddb46493cfaffc5764b2ea3db73428b651c
tts-kristin MODEL_CARD 479 8c181b1d5f7d8152b914faab941fcbe6b8f0495e63e5189d6f27b8c68c8393fc
tts-thorsten model.onnx 63222140 625289a342bab493905be5f96f29d7ddba50c5e4e0b59d37d68ee23ef45b66e5
tts-thorsten tokens.txt 1008 d9cda511aa50e7bdfb83258ff24c58df701ca1ad339db1e1ab0e614d52e26fec
tts-thorsten clusters.txt 41 a55bc25aeeb5e3fc58bc9e664d59a5214cee66ae03e8f09b39ff13e91dfbc9b6
tts-thorsten model.onnx.json 6455 e5bb7410dcbbcc0be6f4544f6bc66612b315ab5cb3099ec44bb46d860bc7cca9
tts-thorsten MODEL_CARD 427 c638118fb06eb07d59e60270097c250cf2de81a037320a8a6306fec652ad13b0
tts-eva_k model.onnx 20628969 0dcc00eacc0c604fdbf2fd5822ca4dd789f3bc2048e5748f21e4c68de61d665a
tts-eva_k tokens.txt 763 42d1a69ed2b91a51928a711aa228ed9f3dc021c6d359a3e9c4f37eb1d20f80bd
tts-eva_k clusters.txt 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
tts-eva_k model.onnx.json 4158 ef14b3dcb279ab4b18422a7a132877bee7a148821bd91152fb7ae9c4b3d79625
tts-eva_k MODEL_CARD 246 187741f947241c18a8d8cdf773fcec75325172628442a866c2da44a5f23e590f
EOF
}

usage() {
	sed -n '/^# Usage:/,/^#$/ { s/^# \{0,1\}//; p; }' "$0" >&2
	exit 2
}

log() { # also to the kernel log, where the other services report
	echo "lidar-models: $*"
	{ echo "lidar-models: $*" > /dev/kmsg; } 2>/dev/null
	return 0
}

progress() { # state message; the percentage is $PCT
	[ -n "$INSTALLING" ] || return 0
	{
		printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$PCT" "$LNG" "$VOICE" "$2" > "$PROGRESS.tmp" &&
			mv "$PROGRESS.tmp" "$PROGRESS"
	} 2>/dev/null
	return 0
}

die() {
	echo "lidar-models: $*" >&2
	FAILED=1
	progress error "$*"
	exit 1
}

fail() { # die during install, naming a full disk as the reason
	[ "$(free_bytes "$DIR")" -ge 1048576 ] || die "no space left in $DIR ($*)"
	die "$@"
}

mb() { echo $((($1 + 1048575) >> 20)); }
sum_of() { sha256sum < "$1" | cut -d' ' -f1; }
size_of() { stat -c %s "$1" 2>/dev/null || echo 0; }
free_bytes() { df -Pk "$1" | awk 'NR == 2 { printf "%.0f\n", $4 * 1024 }'; }
bytes_of() { "$1" | awk -v c="$2" '$1 == c { n += $3 - 0 } END { printf "%.0f\n", n }'; }
src_bytes() { sources | awk -v c="$1" '$1 == c { n += $2 } END { printf "%.0f\n", n }'; }

voice_row() { # language [voice] -> the row of voices, empty if there is none
	voices | awk -v l="$1" -v v="$2" '$1 == l && (v == "" || $2 == v) { print; exit }'
}

# The installed language: CUR (directory name), CUR_LANG, CUR_VOICE
current() {
	CUR=$(readlink "$DIR/current" 2>/dev/null)
	CUR_LANG=${CUR%%-*}
	CUR_VOICE=${CUR#*-}
	[ -n "$CUR" ] && [ -d "$DIR/$CUR" ] && [ -n "$(voice_row "$CUR_LANG" "$CUR_VOICE")" ] ||
		CUR='' CUR_LANG='' CUR_VOICE=''
}

check() { # component directory [quiet]: every pinned file is there and intact
	bad=0
	while read -r _ f n sum; do
		if [ ! -f "$2/$f" ]; then
			[ -n "$3" ] || echo "$2/$f: missing" >&2
			bad=1
		elif [ "$(size_of "$2/$f")" != "$n" ] || [ "$(sum_of "$2/$f")" != "$sum" ]; then
			[ -n "$3" ] || echo "$2/$f: does not match its sha256" >&2
			bad=1
		fi
	done <<EOF
$(files | awk -v c="$1" '$1 == c')
EOF
	return $bad
}

lock() { # one lidar-models at a time may change <dir>
	mkdir -p "$DIR" || die "$DIR: cannot create"
	exec 9> "$DIR/.lock" || die "$DIR: not writable"
	flock -n 9 || die "another lidar-models is at work in $DIR"
}

# url file bytes sha256: download with progress, then check size and sha256
fetch() {
	wget -q -T 30 -O "$2" "$1" 9>&- &
	WGET=$!
	while kill -0 "$WGET" 2>/dev/null; do
		PCT=$(((GOT + $(size_of "$2")) * 95 / TOTAL))
		progress download "${1##*/}"
		sleep 1
	done
	wait "$WGET"
	rc=$?
	WGET=
	[ $rc -eq 0 ] || fail "download failed: $1 (network? clock: $(date -u '+%Y-%m-%d %H:%M') UTC)"
	progress verify "${1##*/}"
	[ "$(size_of "$2")" = "$3" ] && [ "$(sum_of "$2")" = "$4" ] ||
		die "${1##*/}: the download does not match its pinned sha256"
	GOT=$((GOT + $3))
	PCT=$((GOT * 95 / TOTAL))
}

meta() { # key value -> one metadata_props entry of an ONNX ModelProto
	# field 14, a message of two strings (fields 1 and 2); all lengths < 128
	printf "\\162\\$(printf %03o $((${#1} + ${#2} + 4)))\\012\\$(printf %03o ${#1})%s\\022\\$(printf %03o ${#2})%s" "$1" "$2"
}

add_metadata() { # model.onnx: what sherpa-onnx' scripts/piper/add_meta_data.py adds
	{
		meta model_type vits
		meta comment piper
		meta language "$LNAME"
		meta voice "$ESPEAK"
		meta version 1
		meta has_espeak 1
		meta has_g2pw 0
		meta n_speakers 1
		meta sample_rate "$RATE"
	} >> "$1"
}

# model.onnx.json tokens.txt clusters.txt: the phoneme_id_map as sherpa-onnx
# reads it. Works on bytes: a token is one code point unless it has more
# than one byte that is not a UTF-8 continuation byte.
gen_tokens() {
	LC_ALL=C awk -v tok="$2" -v clu="$3" '
	{ s = s $0 "\n" }
	END {
		i = index(s, "\"phoneme_id_map\"")
		if (!i)
			exit 1
		s = substr(s, i + 16)
		s = substr(s, index(s, "{") + 1)
		for (n = 128; n < 192; n++)
			cont[sprintf("%c", n)] = 1
		printf "" > tok
		printf "" > clu
		for (;;) {
			sub(/^[ \t\r\n,]+/, "", s)
			if (substr(s, 1, 1) == "}")
				break
			if (substr(s, 1, 1) != "\"")
				exit 1
			key = ""
			chars = 0
			for (i = 2; (c = substr(s, i, 1)) != "\""; i++) {
				if (c == "")
					exit 1
				if (c == "\\") {
					c = substr(s, ++i, 1)
					if (c == "n")
						c = "\n"
					else if (c != "\"" && c != "\\" && c != "/")
						exit 1
				}
				if (!(c in cont))
					chars++
				key = key c
			}
			s = substr(s, i + 1)
			if (!match(s, /^[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*[0-9]+/))
				exit 1
			id = substr(s, 1, RLENGTH)
			sub(/^[^0-9]*/, "", id)
			s = substr(s, RLENGTH + 1)
			s = substr(s, index(s, "]") + 1)
			if (key == "\n")
				continue
			if (chars > 1) {
				if (pua > 63)
					exit 1
				p = sprintf("\356\200%c", 128 + pua++)
				print key, p > clu
				key = p
			}
			print key, id > tok
		}
	}' "$1"
}

cleanup() {
	[ -z "$WGET" ] || kill "$WGET" 2>/dev/null
	[ -z "$NEW" ] || rm -rf "$NEW"
	[ -n "$FINISHED$FAILED" ] || progress error interrupted
}

install() {
	row=$(voice_row "$1" "$VOICE")
	[ -n "$row" ] || die "no voice '${VOICE:-?}' for language '$1' (voices: $(voices | awk '{ printf "%s %s%s", $1, $2, ", " }' | sed 's/, $//'))"
	set -- $row
	LNG=$1 VOICE=$2 LNAME=$3 ESPEAK=$4 RATE=$5 NOISE=$6 NOISE_W=$7 LENGTH=$8
	STT=stt-$LNG TTS=tts-$VOICE NAME=$LNG-$VOICE
	lock
	INSTALLING=1 PCT=0 GOT=0 FINISHED='' FAILED=''
	NEW=$DIR/.new
	trap cleanup EXIT
	trap 'exit 1' INT TERM HUP
	progress verify "checking what is installed"
	current
	if [ "$CUR" = "$NAME" ] && check "$STT" "$DIR/$CUR/stt" quiet && check "$TTS" "$DIR/$CUR/tts" quiet; then
		NEW=
		PCT=100 FINISHED=1
		progress done "$LNG $VOICE"
		echo "$LNG ($VOICE) is installed already"
		return 0
	fi

	case $(stat -f -c %T "$DIR" 2>/dev/null) in
	tmpfs | ramfs)
		[ -n "$IN_RAM" ] || die "$DIR is in RAM, not on flash (booted slot not mounted, or 'lidar-feature keep $DIR' not run)"
		;;
	esac
	# the recognizer of the same language is kept: hard links, no download
	keep_stt=
	[ "$CUR_LANG" = "$LNG" ] && check "$STT" "$DIR/$CUR/stt" quiet && keep_stt=1
	stt=$(bytes_of files "$STT") tts=$(bytes_of files "$TTS")
	if [ -n "$keep_stt" ]; then
		TOTAL=$(src_bytes "$TTS")
		need=$((tts + MARGIN))
	else
		# the archive is deleted once unpacked, before the voice arrives
		TOTAL=$(($(src_bytes "$STT") + $(src_bytes "$TTS")))
		a=$(src_bytes "$STT")
		[ "$a" -gt "$tts" ] || a=$tts
		need=$((stt + a + MARGIN))
	fi
	rm -rf "$NEW" "$DIR/.current"
	free=$(free_bytes "$DIR")
	if [ "$free" -lt "$need" ]; then
		hint=
		[ -z "$CUR" ] || [ -n "$keep_stt" ] ||
			hint=" The installed language ($CUR_LANG) stays until the new one is complete: 'lidar-models remove $CUR_LANG' frees $(mb "$(($(bytes_of files "stt-$CUR_LANG") + $(bytes_of files "tts-$CUR_VOICE")))") MB."
		fs_size "$DIR" 2>/dev/null && [ $((PART - FS)) -gt "$MARGIN" ] &&
			hint="$hint The filesystem has $(mb "$FS") MB of a $(mb "$PART") MB partition: 'lidar-models grow-fs' adds the rest."
		die "not enough space in $DIR: $LNG ($VOICE) needs $(mb "$need") MB free, $(mb "$free") MB are.$hint"
	fi

	mkdir -p "$NEW/stt" "$NEW/tts" || die "$DIR: not writable"
	if [ -n "$keep_stt" ]; then
		ln "$DIR/$CUR/stt"/* "$NEW/stt/" || die "cannot link the installed recognizer"
	else
		set -- $(sources | awk -v c="$STT" '$1 == c { print $4, $2, $3 }')
		fetch "$1" "$NEW/stt.tar.bz2" "$2" "$3"
		top=${1##*/}
		top=${top%.tar.bz2}
		progress unpack "${1##*/}"
		tar -xjf "$NEW/stt.tar.bz2" -C "$NEW" $(files | awk -v c="$STT" -v t="$top" '$1 == c { print t "/" $2 }') ||
			fail "${1##*/}: cannot unpack"
		rm -f "$NEW/stt.tar.bz2"
		mv "$NEW/$top"/* "$NEW/stt/" && rmdir "$NEW/$top" || die "${1##*/}: unexpected contents"
	fi
	while read -r _ n sum url; do
		case $url in
		*.onnx) f=model.onnx ;;
		*.onnx.json) f=model.onnx.json ;;
		*) f=${url##*/} ;;
		esac
		fetch "$url" "$NEW/tts/$f" "$n" "$sum"
	done <<EOF
$(sources | awk -v c="$TTS" '$1 == c')
EOF
	progress verify "$LNG $VOICE"
	add_metadata "$NEW/tts/model.onnx" &&
		gen_tokens "$NEW/tts/model.onnx.json" "$NEW/tts/tokens.txt" "$NEW/tts/clusters.txt" ||
		fail "cannot prepare the voice for sherpa-onnx"
	printf '%s\n' "lang=$LNG" "voice=$VOICE" "espeak_voice=$ESPEAK" "sample_rate=$RATE" \
		"noise_scale=$NOISE" "noise_w=$NOISE_W" "length_scale=$LENGTH" > "$NEW/info" || fail "$DIR: write failed"
	check "$STT" "$NEW/stt" && check "$TTS" "$NEW/tts" || die "the installed files do not match their pins"
	sync

	# Switch. A damaged copy under the same name moves out of the way first;
	# everything but the new directory goes afterwards.
	[ ! -e "$DIR/$NAME" ] || { rm -rf "$DIR/.old" && mv "$DIR/$NAME" "$DIR/.old"; } || die "$DIR: cannot replace $NAME"
	mv "$NEW" "$DIR/$NAME" || die "$DIR: cannot rename"
	NEW=
	rm -f "$DIR/.current" && ln -s "$NAME" "$DIR/.current" && mv -T "$DIR/.current" "$DIR/current" ||
		die "$DIR: cannot switch"
	sync
	rm -rf "$DIR/.old"
	for d in "$DIR"/*/; do
		d=${d%/}
		[ "$d" = "$DIR/$NAME" ] || [ -L "$d" ] || rm -rf "$d"
	done
	sync
	PCT=100 FINISHED=1
	progress done "$LNG $VOICE"
	echo "$LNG ($VOICE) installed: $(mb $((stt + tts))) MB in $DIR/$NAME"
}

remove() {
	[ -n "$(voice_row "$1")" ] || die "unknown language '$1' (en or de)"
	lock
	current
	[ "$CUR_LANG" = "$1" ] || die "$1 is not installed"
	rm -f "$DIR/current" && rm -rf "$DIR/${CUR:?}" && sync || die "$DIR: cannot remove"
	echo "$1 removed"
}

verify() {
	current
	[ -n "$CUR" ] || die "nothing installed in $DIR"
	check "stt-$CUR_LANG" "$DIR/$CUR/stt" && check "tts-$CUR_VOICE" "$DIR/$CUR/tts" ||
		die "$CUR_LANG ($CUR_VOICE) is damaged: install it again"
	echo "$CUR_LANG ($CUR_VOICE) is intact"
}

status() {
	current
	echo "dir=$DIR"
	if [ -n "$CUR" ]; then
		echo "installed=$CUR_LANG $CUR_VOICE"
		echo "size_mb=$(mb "$(($(bytes_of files "stt-$CUR_LANG") + $(bytes_of files "tts-$CUR_VOICE")))")"
	else
		echo "installed=none"
	fi
	d=$DIR
	[ -d "$d" ] || d=${d%/*}
	[ ! -d "$d" ] || echo "free_mb=$(($(free_bytes "$d") >> 20))"
	busy=0
	[ ! -e "$DIR/.lock" ] || (flock -n 9 || exit 1) 9< "$DIR/.lock" || busy=1
	echo "busy=$busy"
	if fs_size "$d" 2>/dev/null; then
		echo "fs_mb=$(mb "$FS")"
		echo "partition_mb=$(mb "$PART")"
	fi
	# language, voice, MB to download, MB installed
	voices | while read -r l v _; do
		echo "available=$l $v $(mb "$(($(src_bytes "stt-$l") + $(src_bytes "tts-$v")))") $(mb "$(($(bytes_of files "stt-$l") + $(bytes_of files "tts-$v")))")"
	done
}

le32() { # device offset -> little-endian 32-bit number there
	dd if="$1" bs=1 skip="$2" count=4 2>/dev/null | od -An -tu4 | tr -d ' \n'
}

# directory -> DEV (block device of its filesystem), FS and PART (bytes of
# the ext4 filesystem and of the partition); fails with a message otherwise
fs_size() {
	DEV=$(df -P "$1" 2>/dev/null | awk 'NR == 2 { print $1 }')
	DEV=$(readlink -f "$DEV" 2>/dev/null)
	[ -b "$DEV" ] || { echo "$1 is not on a partition" >&2; return 1; }
	[ "$(dd if="$DEV" bs=1 skip=1080 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 53ef ] ||
		{ echo "$DEV: no ext4 filesystem (or not readable)" >&2; return 1; }
	blocks=$(le32 "$DEV" 1028)
	# high half of the block count: only with the 64bit feature
	[ $(($(le32 "$DEV" 1120) & 0x80)) -eq 0 ] || blocks=$((blocks + ($(le32 "$DEV" 1360) << 32)))
	FS=$((blocks * (1024 << $(le32 "$DEV" 1048))))
	PART=$(cat "/sys/class/block/${DEV##*/}/size" 2>/dev/null) || { echo "$DEV: size unknown" >&2; return 1; }
	PART=$((PART * 512))
}

grow_fs() {
	command -v resize2fs > /dev/null || die "resize2fs is missing (Alpine package e2fsprogs-extra)"
	[ -d "$DIR" ] || die "$DIR: no such directory"
	msg=$(fs_size "$DIR" 2>&1) || die "$msg"
	fs_size "$DIR"
	log "grow-fs: $DEV holds $(mb "$FS") MB ($FS bytes) on a partition of $(mb "$PART") MB ($PART bytes)"
	[ "$PART" -ge "$FS" ] || die "grow-fs: the filesystem is larger than its partition; not touching it"
	# less than a block group more (1 KiB blocks: 8 MB) is nothing to add
	if [ $((PART - FS)) -lt "$MARGIN" ]; then
		log "grow-fs: the filesystem fills its partition already"
		return 0
	fi
	# resize2fs resizes through the first mount of the device, which has to
	# be writable; the booted slot is mounted read-only, wholly or only at
	# that mount (lidar-feature keep). Same lock and same way back as there.
	exec 8> "$SLOT_LOCK" && flock 8 || die "grow-fs: cannot lock $SLOT_LOCK"
	set -- $(stat -c '%t %T' "$DEV")
	set -- $(awk -v d="$((0x$1)):$((0x$2))" '$3 == d { split($0, a, " - "); split(a[2], s, " "); print $5, s[1], $6, s[3]; exit }' /proc/self/mountinfo)
	[ -n "$1" ] || die "grow-fs: $DEV is not mounted (only a mounted filesystem is grown)"
	[ "$2" = ext4 ] || die "grow-fs: $DEV is mounted as $2, not ext4"
	mnt=$1 ro=
	# whatever ends this, the mount goes back to read-only
	trap 'sync; [ -z "$ro" ] || mount -o "$ro" "$mnt"' EXIT
	trap 'exit 1' INT TERM HUP
	case ",$3," in *,ro,*)
		case ",$4," in
		*,ro,*) mount -o remount,rw "$mnt" && ro=remount,ro ;;
		*) mount -o remount,bind,rw "$mnt" && ro=remount,bind,ro ;;
		esac || die "grow-fs: cannot make $mnt writable"
		;;
	esac
	sync
	out=$(resize2fs "$DEV" 2>&1)
	rc=$?
	sync
	[ -z "$ro" ] || mount -o "$ro" "$mnt" || log "grow-fs: $mnt stays writable (remount failed)"
	ro=
	echo "$out"
	[ $rc -eq 0 ] || die "grow-fs: resize2fs failed with status $rc; the filesystem keeps the size it reached (see dmesg)"
	old=$FS
	fs_size "$DIR" || die "grow-fs: cannot read the filesystem back"
	[ "$FS" -gt "$old" ] || die "grow-fs: resize2fs reported success but the filesystem still has $FS bytes"
	log "grow-fs: $DEV grown from $(mb "$old") MB to $(mb "$FS") MB ($FS bytes); $(($(free_bytes "$DIR") >> 20)) MB free"
}

VOICE='' IN_RAM='' INSTALLING='' NEW='' WGET='' cmd='' arg=''
while [ $# -gt 0 ]; do
	case $1 in
	-d) [ $# -ge 2 ] || usage; DIR=${2%/}; shift ;;
	-p) [ $# -ge 2 ] || usage; PROGRESS=$2; shift ;;
	-v) [ $# -ge 2 ] || usage; VOICE=$2; shift ;;
	-R) IN_RAM=1 ;;
	-*) usage ;;
	*)
		if [ -z "$cmd" ]; then cmd=$1
		elif [ -z "$arg" ]; then arg=$1
		else usage
		fi
		;;
	esac
	shift
done
[ -n "$DIR" ] || usage
case $cmd in
status | verify | grow-fs) [ -z "$arg" ] || usage ;;
install | remove) [ -n "$arg" ] || usage ;;
*) usage ;;
esac
case $cmd in
status) status ;;
verify) verify ;;
grow-fs) grow_fs ;;
install) install "$arg" ;;
remove) remove "$arg" ;;
esac
