#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Web UI backend (BusyBox httpd CGI): device statistics, volume, the song
# library and the player, network and radio status and control, logs, files
# and the services. One query per request, JSON out; see the README.
#
# Reads are GET. Commands must be POST with Content-Type application/json,
# and those that carry data (uploads, Wi-Fi credentials, setting values) POST
# with application/octet-stream: a web page elsewhere in the host's browser
# cannot send either without a CORS preflight, which this server never
# answers. The Host header must be an IP address, which keeps DNS rebinding
# out.
#
# There is no login. Everything that changes the device or reads more than
# its status (radios, services, settings, logs, files, reboot) is "admin" and
# only answered for clients on the USB network, even with "webui listen any".
LIB=/run/lidar-player
FIFO=/run/lidar-player.fifo

HDR=
hdr() {
	[ -n "$HDR" ] || printf 'Content-Type: application/json\r\nCache-Control: no-store\r\nCross-Origin-Resource-Policy: same-origin\r\n\r\n'
	HDR=1
}

fail() { hdr; printf '{"error": "%s"}\n' "$1"; exit 0; }
ok() { hdr; printf '{"ok": true}\n'; exit 0; }

# Clients on the USB network: a 169.254.0.0/16 address (httpd writes IPv6
# peers as [addr], IPv4 ones on its dual-stack socket as ::ffff:addr) whose
# route is usb0. Replies to it then only ever leave through usb0, so a Wi-Fi
# host that fakes such a source cannot complete a connection; without usb0
# ("webui listen any") they would follow the default route, hence the check.
admin() {
	a=${REMOTE_ADDR#[}; a=${a%]}; a=${a#::ffff:}
	case $a in 169.254.*) ;; *) fail "only over the USB network" ;; esac
	ip route get "$a" 2>/dev/null | grep -q ' dev usb0 ' || fail "only over the USB network"
}

# JSON string body: backslash and quote escaped, control characters dropped
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\000-\037'; }

# Tab-separated lines -> the same with backslashes and quotes escaped and
# control characters (tabs and newlines kept) dropped, ready for "%s" in JSON
jesc() { tr -d '\000-\010\013-\037\177' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

# Text -> one JSON string, lines joined with \n
jtext() {
	tr '\t' ' ' | jesc | awk 'BEGIN { printf "\"" } { printf "%s\\n", $0 } END { printf "\"" }'
}

# file -> $v (first line); $2 (default null) if unreadable or empty; no fork
rdv() {
	v=
	[ -r "$1" ] && read -r v < "$1"
	[ -n "$v" ] || v=${2-null}
}

player() { # the player's own state file, or stopped
	if [ -r /run/lidar-player.json ]; then
		cat /run/lidar-player.json
	else
		echo '{"state": "stopped", "id": null, "title": null, "elapsed": 0, "duration": 0}'
	fi
}

send() { # command -> player
	[ -p $FIFO ] || fail "player not running"
	echo "$*" > $FIFO
	ok
}

limit_bytes() {
	mb=$(lidar-feature get webui storage_mb 2>/dev/null)
	echo $(( ${mb:-256} * 1024 * 1024 ))
}

used_bytes() {
	u=$(du -sb $LIB 2>/dev/null | cut -f1)
	echo "${u:-0}"
}

valid_id() { case $1 in '' | *[!0-9-]*) return 1 ;; esac; }

stats() {
	read -r up _ < /proc/uptime
	read -r l1 l5 l15 _ < /proc/loadavg
	read -r _ u n s i w q sq _ < /proc/stat
	model=$(tr -d '\000' < /proc/device-tree/model)
	printf '{"time": %s, "uptime": %s, "model": "%s", "kernel": "%s",\n' \
		"$(date +%s)" "$up" "$(esc "$model")" "$(uname -r)"
	printf ' "load": [%s, %s, %s],\n' "$l1" "$l5" "$l15"
	f=
	for c in /sys/devices/system/cpu/cpu[0-9]*; do
		rdv $c/cpufreq/scaling_cur_freq
		f="$f${f:+, }$v"
	done
	rdv /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq; mx=$v
	rdv /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ''; gov=$v
	printf ' "cpu": {"jiffies": [%s, %s, %s, %s, %s, %s, %s], "freq_khz": [%s], "max_khz": %s, "governor": "%s"},\n' \
		"$u" "$n" "$s" "$i" "$w" "$q" "$sq" "$f" "$mx" "$(esc "$gov")"
	awk '/^MemTotal:/ { t = $2 } /^MemAvailable:/ { a = $2 }
		END { printf " \"mem\": {\"total_kb\": %d, \"available_kb\": %d},\n", t, a }' /proc/meminfo
	# board temperature sensors: "<chip> <bus-addr> <millidegrees>" per line
	t=
	for h in /sys/class/hwmon/hwmon*; do
		[ -r $h/temp1_input ] || continue
		rdv $h/name '?'; chip=$v
		rdv $h/temp1_input; mc=$v
		dev=$(readlink -f $h/device)
		t="$t$chip ${dev##*/} $mc
"
	done
	printf '%s' "$t" | awk 'BEGIN { printf " \"temps\": [" }
		NF == 3 { printf "%s{\"name\": \"%s (i2c%s 0x%s)\", \"c\": %s}", n++ ? ", " : "",
			toupper($1), substr($2, 1, index($2, "-") - 1), substr($2, index($2, "-") + 3),
			$3 == "null" ? "null" : sprintf("%.2f", $3 / 1000) }
		END { print "]," }'
	light=null
	for d in /sys/bus/iio/devices/iio:device*; do
		rdv $d/name ''
		[ "$v" = tsl2540 ] || continue
		rdv $d/in_illuminance_raw; vis=$v
		rdv $d/in_intensity_ir_raw; ir=$v
		light="{\"visible\": $vis, \"ir\": $ir}"
	done
	printf ' "light": %s,\n' "$light"
	r=
	for g in /sys/class/regulator/regulator.*; do
		rdv $g/microvolts 0
		[ "$v" != 0 ] || continue	# unreadable, or the dummy regulator
		uv=$v
		rdv $g/name ''
		r="$r${r:+, }{\"name\": \"$(esc "$v")\", \"mv\": $((uv / 1000))}"
	done
	printf ' "rails": [%s],\n' "$r"
	vol=null
	if [ -r /run/lidar-volume.state ]; then
		read -r level mute vmax < /run/lidar-volume.state
		m=false; [ "$mute" = 1 ] && m=true
		[ "${vmax:-0}" -gt 0 ] && vol=$(awk -v l="$level" -v v="$vmax" -v m=$m 'BEGIN {
			printf "{\"percent\": %d, \"muted\": %s, \"db\": %.1f}", (l * 100 + v / 2) / v, m, (l - 127) / 2 }')
	fi
	printf ' "volume": %s,\n' "$vol"
	mic=false
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	if [ -n "$ev" ]; then
		evtest --query /dev/input/$ev EV_SW SW_MUTE_DEVICE >/dev/null 2>&1
		[ $? = 10 ] && mic=true
	fi
	dsp=false; [ -e /run/lidar-dsp/ready ] && dsp=true
	printf ' "mic_muted": %s, "dsp": %s,\n' "$mic" "$dsp"
	# LED ring: 12 x red, green, blue, scaled to 0..255, one awk for all
	files=
	for k in 1 2 3 4 5 6 7 8 9 10 11 12; do
		for c in red green blue; do
			files="$files /sys/class/leds/ring$k:$c/brightness"
		done
	done
	rdv /sys/class/leds/ring1:red/max_brightness 255; mb=$v
	# shellcheck disable=SC2086 # the list is meant to split
	awk -v mb="$mb" 'BEGIN { if (mb <= 0) mb = 255 } FNR == 1 { b[n++] = int($1 * 255 / mb) }
		END { printf " \"ring\": ["
		      for (i = 0; i < 36; i += 3) printf "%s[%d, %d, %d]", i ? ", " : "", b[i], b[i + 1], b[i + 2]
		      print "]," }' $files 2>/dev/null
	rdv /sys/class/net/usb0/statistics/rx_bytes; rx=$v
	rdv /sys/class/net/usb0/statistics/tx_bytes; tx=$v
	printf ' "net": {"rx_bytes": %s, "tx_bytes": %s},\n' "$rx" "$tx"
	printf ' "player": %s}\n' "$(player)"
}

services() {
	sv=
	while read -r name group on _ _ desc; do
		[ -n "$name" ] || continue
		e=false; [ "$on" = on ] && e=true
		sv="$sv${sv:+, }{\"name\": \"$(esc "$name")\", \"group\": \"$(esc "$group")\", \"enabled\": $e, \"description\": \"$(esc "$desc")\"}"
	done <<-EOF
	$(lidar-feature list)
	EOF
	printf '{"services": [%s]}\n' "$sv"
}

songs() {
	s=
	for f in $LIB/*.wav; do
		[ -e "$f" ] || continue
		id=${f##*/}; id=${id%.wav}
		rdv $LIB/$id.title ''; title=$v
		rdv $LIB/$id.duration 0; d=$v
		s="$s${s:+,
 }{\"id\": \"$id\", \"title\": \"$(esc "$title")\", \"duration\": $d, \"bytes\": $(stat -c %s "$f" 2>/dev/null || echo 0)}"
	done
	printf '{"songs": [%s],\n "used_bytes": %s, "limit_bytes": %s}\n' "$s" "$(used_bytes)" "$(limit_bytes)"
}

upload() {
	len=${CONTENT_LENGTH:-0}
	case $len in '' | *[!0-9]*) fail "bad length" ;; esac
	[ "$len" -gt 44 ] || fail "empty upload"
	[ -d $LIB ] || fail "player not running"
	# leftovers of uploads whose CGI died
	for f in $LIB/.upload.*; do
		[ -e "$f" ] && ! kill -0 "${f##*-}" 2>/dev/null && rm -f "$f"
	done
	limit=$(limit_bytes)
	# 64 KB stay free for the small title and duration files
	[ $(( $(used_bytes) + len + 65536 )) -le "$limit" ] ||
		fail "library full (limit $((limit / 1024 / 1024)) MB)"
	id=$(printf '%012d-%05d' "$(date +%s)" $$)
	tmp=$LIB/.upload.$id
	trap 'rm -f "$tmp"' EXIT
	head -c "$len" > "$tmp"
	# short: the client went away, or the library's tmpfs is full
	[ "$(stat -c %s "$tmp")" = "$len" ] || fail "incomplete upload (library full?)"
	d=$(lidar-player info "$tmp" 2>/dev/null) || fail "not a 48 kHz 16-bit stereo PCM WAV"
	t=$(busybox-extras httpd -d "$title" | tr -d '\000-\037' | cut -c1-200)
	printf '%s\n' "${t:-Untitled}" > $LIB/$id.title
	echo "$d" > $LIB/$id.duration
	mv "$tmp" $LIB/$id.wav
	trap - EXIT
	printf '{"ok": true, "id": "%s"}\n' "$id"
}

# ---------- network and radios ----------

wifi_json() {
	lidar-wifi status 2>/dev/null | jesc | awk -F= '
		function s(k) { return (k in v && v[k] != "") ? "\"" v[k] "\"" : "null" }
		function n(k) { return (k in v && v[k] != "") ? v[k] + 0 : "null" }
		{ k = $1; x = substr($0, length(k) + 2) }
		k == "ipv4" || k == "ipv6" { a[k] = a[k] (a[k] == "" ? "" : ", ") "\"" x "\""; next }
		{ v[k] = x }
		END {
			printf "{\"state\": %s, \"ssid\": %s, \"configured\": %s, \"freq\": %s, \"signal\": %s, \"bitrate\": %s, \"ipv4\": [%s], \"ipv6\": [%s]}",
				s("state"), s("ssid"), s("configured"), n("freq"), n("signal"), n("bitrate"), a["ipv4"], a["ipv6"]
		}'
}

bt_json() {
	if [ ! -d /sys/class/bluetooth/hci0 ]; then
		printf '{"present": false}'
		return
	fi
	# speaker: lidar-bluetooth's daemon runs; pairing: its pairing mode is on
	sp=false; [ -p /run/lidar-bluetooth.fifo ] && pgrep -f 'lidar-bluetooth daemon' >/dev/null && sp=true
	pm=false; [ -e /run/lidar-bluetooth.pairing ] && pm=true
	timeout 2 btmgmt info 2>/dev/null | tr '\t' ' ' | jesc | awk -v sp=$sp -v pm=$pm '
		/^[[:space:]]+addr / { addr = $2 }
		/current settings:/ { pw = / powered( |$)/ }
		/^[[:space:]]+name / { sub(/^[[:space:]]+name /, ""); name = $0 }
		END { printf "{\"present\": true, \"powered\": %s, \"address\": \"%s\", \"name\": \"%s\", \"speaker\": %s, \"pairing\": %s}",
			pw ? "true" : "false", addr, name, sp, pm }'
}

# Phones paired with the speaker (lidar-bluetooth devices: address,
# connected, name; tab-separated, the name last)
bt_devices() {
	lidar-bluetooth devices 2>/dev/null | jesc | awk -F '\t' 'BEGIN { printf "{\"devices\": [" }
		NF >= 3 { n = $0; sub(/^[^\t]*\t[^\t]*\t/, "", n); gsub(/\t/, " ", n)
			printf "%s{\"address\": \"%s\", \"connected\": %s, \"name\": \"%s\"}",
				c++ ? ", " : "", $1, $2 == "yes" ? "true" : "false", n }
		END { print "]}" }'
}

# The first line of the agent's answer to a CLI command. Not ot-ctl: it exits
# after "Done" with the prompt unread, and the agent logs every such call as
# a reset connection. socat closes its side first and reads to the end.
otc() {
	o=$(printf '%s\n' "$*" | timeout 3 socat -t 1 - UNIX-CONNECT:/run/openthread-wpan0.sock 2>/dev/null) || return
	printf '%s\n' "${o%%
*}" | jesc
}

radio_json() {
	t=null
	# a stalled agent costs one timeout, not five
	if pgrep -x otbr-agent >/dev/null && st=$(otc state) && [ -n "$st" ]; then
		t="{\"state\": \"$st\", \"channel\": \"$(otc channel)\", \"network\": \"$(otc networkname)\", \"panid\": \"$(otc panid)\", \"border_routing\": \"$(otc br state)\"}"
	fi
	te=false; lidar-feature enabled thread && te=true
	ze=false; lidar-feature enabled zigbee && ze=true
	zb=false; pgrep -x zigbeed >/dev/null && zb=true
	printf '"thread": {"enabled": %s, "agent": %s}, "zigbee": {"enabled": %s, "bridge": %s}' "$te" "$t" "$ze" "$zb"
}

network() {
	printf '{"wifi": %s, "bluetooth": %s, %s}\n' "$(wifi_json)" "$(bt_json)" "$(radio_json)"
}

wifi_scan() {
	lidar-wifi scan 2>/dev/null | jesc | awk -F '\t' 'BEGIN { printf "{\"networks\": [" }
		NF >= 4 { printf "%s{\"signal\": %s, \"bands\": \"%s\", \"security\": \"%s\", \"ssid\": \"%s\"}",
			n++ ? ", " : "", $1 + 0, $2, $3, $4 }
		END { print "]}" }'
}

body_len() { # at most $1 bytes of request body -> $len; in this shell, so fail ends the request
	len=${CONTENT_LENGTH:-0}
	case $len in '' | *[!0-9]*) fail "bad length" ;; esac
	[ "$len" -le "$1" ] || fail "request too long"
}

# body: SSID, newline, passphrase
wifi_connect() {
	body_len 200
	b=$(head -c "$len")
	ssid=${b%%
*}
	pass=${b#*
}
	[ "$ssid" != "$b" ] || fail "expected the SSID and the passphrase on two lines"
	# checked before anything is saved: a new SSID with the old passphrase
	# would join nothing at the next boot
	case $pass in *[![:print:]]*) pass= ;; esac
	[ ${#pass} -ge 8 ] && [ ${#pass} -le 63 ] ||
		fail "invalid passphrase (8-63 printable ASCII characters)"
	lidar-feature enabled wifi || fail "the wifi service is off (enable it, then reboot)"
	lidar-feature set wifi ssid "$ssid" >/dev/null 2>&1 || fail "invalid SSID (1-32 bytes)"
	lidar-feature set wifi passphrase "$pass" >/dev/null 2>&1 ||
		fail "invalid passphrase (8-63 printable ASCII characters)"
	lidar-wifi restart >/dev/null 2>&1 || fail "Wi-Fi did not start (see the kernel log)"
	ok
}

bt_scan() {
	[ -d /sys/class/bluetooth/hci0 ] || fail "no Bluetooth controller"
	# "hci0 dev_found: <addr> type <LE Public|LE Random|BR/EDR> rssi <n> flags ...",
	# then the advertised "name <name>"; discovery ends by itself
	timeout 15 btmgmt find 2>/dev/null | tr '\t' ' ' | jesc | awk '
		/dev_found: / {
			a = $0; sub(/.*dev_found: /, "", a); cur = a; sub(/ .*/, "", cur)
			t = a; sub(/^[^ ]+ type /, "", t); sub(/ rssi .*/, "", t)
			r = a; sub(/.* rssi /, "", r); r = r + 0
			if (!(cur in rssi) || r > rssi[cur]) rssi[cur] = r
			type[cur] = t
			next
		}
		/^[[:space:]]*name / && cur != "" { nm = $0; sub(/^[[:space:]]*name /, "", nm); name[cur] = nm }
		END {
			printf "{\"devices\": ["
			for (d in rssi) printf "%s{\"address\": \"%s\", \"type\": \"%s\", \"rssi\": %d, \"name\": \"%s\"}",
				n++ ? ", " : "", d, type[d], rssi[d], name[d]
			print "]}"
		}'
}

# ---------- debug ----------

klog() { # this boot's kernel log, or the previous boot's from pstore
	if [ "$val" = previous ]; then
		f=$(ls /sys/fs/pstore/console-ramoops* /sys/fs/pstore/dmesg-ramoops* 2>/dev/null | head -n 1)
		[ -n "$f" ] || { printf '{"text": null}\n'; return; }
		printf '{"text": %s}\n' "$(tail -n 3000 "$f" | jtext)"
	else
		printf '{"text": %s}\n' "$(dmesg | tail -n 3000 | jtext)"
	fi
}

system_info() {
	printf '{"text": %s}\n' "$({
		echo '# df -h'; df -h
		echo; echo '# mounts'; cat /proc/mounts
		echo; echo '# lsmod'; lsmod
	} 2>&1 | jtext)"
}

# ---------- services and settings ----------

settings() {
	lidar-feature settings --raw | jesc | awk -F '\t' 'BEGIN { printf "{\"settings\": [" }
		NF >= 6 { printf "%s{\"feature\": \"%s\", \"key\": \"%s\", \"value\": \"%s\", \"default\": \"%s\", \"allowed\": \"%s\", \"description\": \"%s\"}",
			n++ ? ",\n " : "", $1, $2, $3, $4, $5, $6 }
		END { print "]}" }'
}

setting_name() { # $val = <feature>.<key> -> $sf, $sk
	case $val in *.* ) ;; *) fail "bad setting" ;; esac
	sf=${val%%.*}; sk=${val#*.}
	case $sf$sk in '' | *[!a-z0-9_]*) fail "bad setting" ;; esac
}

# body: the new value
set_setting() {
	setting_name
	body_len 256
	v=$(head -c "$len")
	msg=$(lidar-feature set "$sf" "$sk" "$v" 2>&1 >/dev/null) || fail "$(esc "$msg")"
	ok
}

# ---------- files (read-only) ----------

fpath() { # URL-encoded absolute path -> $p, canonical
	p=$(busybox-extras httpd -d "$1")
	case $p in /*) ;; *) fail "bad path" ;; esac
	p=$(realpath "$p" 2>/dev/null) || fail "no such path"
}

ls_dir() {
	fpath "$val"
	[ -d "$p" ] || fail "not a directory"
	printf '{"path": "%s", "entries": [' "$(esc "$p")"
	# type|size|mtime|path; the path may hold "|", so it is what follows the third
	find "$p" -mindepth 1 -maxdepth 1 -exec stat -L -c '%F|%s|%Y|%n' {} + 2>/dev/null | tr '\t' ' ' | jesc | awk -F '|' '{
		n = $0; sub(/^[^|]*\|[^|]*\|[^|]*\|/, "", n); sub(/.*\//, "", n)
		t = $1 == "directory" ? "dir" : ($1 ~ /regular/ ? "file" : "other")
		printf "%s{\"name\": \"%s\", \"type\": \"%s\", \"size\": %d, \"mtime\": %d}", c++ ? ",\n " : "", n, t, $2, $3 }'
	printf ']}\n'
}

# Download, or with "&view" the first 256 KB as plain text. Device nodes,
# the kernel message stream, process memory and debugfs (whose register
# dumps can stall the bus on clock-gated blocks) are refused.
get_file() {
	fpath "$val"
	case $p in
	/dev/* | /proc/kmsg | /proc/kcore | /proc/*/mem | /proc/*/pagemap | /sys/kernel/debug/*)
		fail "not readable from here" ;;
	esac
	[ -f "$p" ] || fail "not a regular file"
	[ -r "$p" ] || fail "not readable"
	n=$(printf '%s' "${p##*/}" | tr -c 'A-Za-z0-9._-' '_')
	case $QUERY_STRING in
	*'&view'*)
		printf 'Content-Type: text/plain; charset=utf-8\r\nX-Content-Type-Options: nosniff\r\nCross-Origin-Resource-Policy: same-origin\r\nCache-Control: no-store\r\n\r\n'
		timeout 5 head -c 262144 "$p"
		;;
	*)
		printf 'Content-Type: application/octet-stream\r\nContent-Disposition: attachment; filename="%s"\r\nX-Content-Type-Options: nosniff\r\nCross-Origin-Resource-Policy: same-origin\r\nCache-Control: no-store\r\n\r\n' "$n"
		timeout 60 cat "$p"
		;;
	esac
}

# the server is reached by its address, never by a name: a.b.c.d[:port] or
# [IPv6][:port]
case $HTTP_HOST in
\[*) h=${HTTP_HOST#[}; h=${h%%]*} ;;
*) h=${HTTP_HOST%:*}; case $h in *[!0-9.]*) h= ;; esac ;;
esac
case $h in '' | *[!0-9A-Za-z:.%]*) fail "bad host" ;; esac

# QUERY_STRING: one command, e.g. "stats", "play=<id>", "upload&title=..."
q=${QUERY_STRING%%&*}
key=${q%%=*}
val=; [ "$key" != "$q" ] && val=${q#*=}
title=
case $QUERY_STRING in *'&title='*) title=${QUERY_STRING#*&title=}; title=${title%%&*} ;; esac

# downloads write their own headers
[ "$key" = file ] || hdr

case $key in
stats | player | songs | services | network) ;;
wifi_scan | bt_scan | bt_devices | log | procs | system | settings | ls | file) admin ;;
upload | wifi_connect | set)
	[ "$REQUEST_METHOD" = POST ] && [ "$CONTENT_TYPE" = application/octet-stream ] ||
		fail "$key needs POST with application/octet-stream"
	[ "$key" = upload ] || admin
	;;
*)
	[ "$REQUEST_METHOD" = POST ] && [ "$CONTENT_TYPE" = application/json ] ||
		fail "commands need POST with application/json"
	case $key in wifi_forget | bt_power | bt_pair | bt_forget | enable | disable | unset | reboot) admin ;; esac
	;;
esac

case $key in
stats) stats ;;
player) player ;;
songs) songs ;;
services) services ;;
upload) upload ;;
play) valid_id "$val" && [ -e $LIB/$val.wav ] || fail "no such song"; send play "$val" ;;
pause | resume | stop | next | prev) send "$key" ;;
seek)
	case $val in '' | *[!0-9.]* | ?????????*) fail "bad position" ;; esac
	send seek "$val"
	;;
delete)
	valid_id "$val" && [ -e $LIB/$val.wav ] || fail "no such song"
	case $(player) in *"\"id\": \"$val\""*) [ -p $FIFO ] && echo stop > $FIFO ;; esac
	rm -f $LIB/$val.wav $LIB/$val.title $LIB/$val.duration
	ok
	;;
volume)
	case $val in '' | *[!0-9]* | ????*) fail "bad volume" ;; esac
	[ "$val" -le 100 ] || fail "bad volume"
	lidar-volume set "$val" >/dev/null 2>&1 || fail "volume service not running"
	ok
	;;
mute)
	case $val in
	1) lidar-volume mute >/dev/null 2>&1 || fail "volume service not running" ;;
	0) lidar-volume unmute >/dev/null 2>&1 || fail "volume service not running" ;;
	*) fail "bad mute" ;;
	esac
	ok
	;;
network) network ;;
wifi_scan) wifi_scan ;;
wifi_connect) wifi_connect ;;
wifi_forget)
	lidar-feature unset wifi ssid >/dev/null && lidar-feature unset wifi passphrase >/dev/null ||
		fail "cannot change the settings"
	lidar-wifi stop
	ok
	;;
bt_power)
	# LE first: bluetoothd turns it on, but may be restarting; a discovery
	# over both transports is rejected without it
	case $val in
	on)
		{ timeout 5 btmgmt le on && timeout 5 btmgmt power on; } >/dev/null 2>&1 ||
			fail "Bluetooth controller not available (service bluetooth)"
		;;
	off) timeout 5 btmgmt power off >/dev/null 2>&1 || fail "Bluetooth controller not available (service bluetooth)" ;;
	*) fail "bad power" ;;
	esac
	ok
	;;
bt_scan) bt_scan ;;
bt_devices) bt_devices ;;
bt_pair)
	case $val in '') a= ;; stop) a=stop ;; *) fail "bad pairing command" ;; esac
	lidar-bluetooth pair $a >/dev/null 2>&1 || fail "Bluetooth speaker not running (service bluetooth)"
	ok
	;;
bt_forget)
	case $val in
	[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]:[0-9A-F][0-9A-F]) ;;
	*) fail "bad address" ;;
	esac
	lidar-bluetooth forget "$val" >/dev/null 2>&1 || fail "cannot remove the phone"
	ok
	;;
log) klog ;;
procs) printf '{"text": %s}\n' "$(top -b -n 1 | head -n 80 | jtext)" ;;
system) system_info ;;
settings) settings ;;
enable | disable)
	case $val in '' | *[!a-z0-9]*) fail "bad service" ;; esac
	lidar-feature "$key" "$val" >/dev/null 2>&1 || fail "cannot change $val"
	ok
	;;
set) set_setting ;;
unset)
	setting_name
	lidar-feature unset "$sf" "$sk" >/dev/null 2>&1 || fail "unknown setting"
	ok
	;;
ls) ls_dir ;;
file) get_file ;;
reboot)
	(sleep 1; reboot) >/dev/null 2>&1 &
	ok
	;;
*) fail "unknown request" ;;
esac
