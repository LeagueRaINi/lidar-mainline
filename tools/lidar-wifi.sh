#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Wi-Fi client on wlan0 from the "wifi ssid" and "wifi passphrase"
# settings (lidar-feature): wpa_supplicant for WPA2-Personal, udhcpc for the
# address. The SSID goes to wpa_supplicant as hex and the passphrase as its
# 64-digit PSK: wpa_supplicant's own quoting breaks on '"' followed by '#'.
# wpa_passphrase only reads the passphrase from a terminal, so it is an
# argument for a moment; everything on this system runs as root anyway.
#
# Usage: lidar-wifi start     join the configured network (nothing if unset;
#                             refused while the wifi service is off)
#        lidar-wifi stop      leave it
#        lidar-wifi restart
#        lidar-wifi status    key=value lines: state, ssid, signal, ...
#        lidar-wifi scan      one network name per line: signal<TAB>bands<TAB>security<TAB>SSID
#                             (bands in GHz, e.g. "2.4 5"; signal of the strongest access point)
IF=wlan0
CONF=/run/wpa_supplicant.conf
WPA_PID=/run/wpa_supplicant.pid
DHCP_PID=/run/udhcpc.$IF.pid

log() { echo "lidar-wifi: $*" > /dev/kmsg; }

wait_if() {
	for i in $(seq 1 20); do
		[ -d /sys/class/net/$IF ] && return 0
		sleep 0.5
	done
	return 1
}

stop() {
	for p in $DHCP_PID $WPA_PID; do
		if [ -r $p ] && pid=$(cat $p) && kill "$pid" 2>/dev/null; then
			# gone before the next start: wpa_supplicant removes its
			# pid file on exit, which may by then be the new one's
			for i in $(seq 1 30); do
				kill -0 "$pid" 2>/dev/null || break
				sleep 0.1
			done
		fi
		rm -f $p
	done
	rm -f $CONF
	[ -d /sys/class/net/$IF ] && ip addr flush dev $IF 2>/dev/null
	return 0
}

start() {
	if ! lidar-feature enabled wifi; then
		log "the wifi service is off"
		echo "lidar-wifi: the wifi service is off" >&2
		return 1
	fi
	ssid=$(lidar-feature get wifi ssid)
	pass=$(lidar-feature get wifi passphrase)
	if [ -z "$ssid$pass" ]; then
		return 0
	elif [ -z "$ssid" ] || [ -z "$pass" ]; then
		log "needs both wifi ssid and wifi passphrase"
		return 1
	fi
	wait_if || { log "no $IF"; return 1; }
	psk=$(wpa_passphrase "$ssid" "$pass" | sed -n 's/^[[:space:]]psk=//p')
	if [ ${#psk} -ne 64 ]; then
		log "wpa_passphrase gave no PSK"
		return 1
	fi
	hex=$(printf %s "$ssid" | od -An -tx1 -v | tr -d ' \n')
	(umask 077; printf 'network={\n\tssid=%s\n\tpsk=%s\n}\n' "$hex" "$psk" > $CONF)
	if ! wpa_supplicant -B -i $IF -c $CONF -P $WPA_PID > /dev/kmsg 2>&1; then
		log "wpa_supplicant failed to start"
		return 1
	fi
	# in the background at once: -b alone waits ~9 s first
	udhcpc -b -i $IF -p $DHCP_PID > /dev/kmsg 2>&1 &
	log "joining the configured network"
}

status() {
	if [ ! -d /sys/class/net/$IF ]; then
		echo state=absent
		return 0
	fi
	# "Connected to <bssid>", then "\tSSID: x", "\tfreq: 5180", "\tsignal: -50 dBm",
	# "\ttx bitrate: 866.7 MBit/s ..."
	iw dev $IF link 2>/dev/null | awk '
		/^Connected to/ { c = 1 }
		/^[[:space:]]*SSID: / { sub(/^[[:space:]]*SSID: /, ""); ssid = $0 }
		/^[[:space:]]*freq: / { freq = $2 }
		/^[[:space:]]*signal: / { sig = $2 }
		/^[[:space:]]*tx bitrate: / { rate = $3 }
		END {
			print "state=" (c ? "connected" : "disconnected")
			if (c) { print "ssid=" ssid; print "freq=" freq; print "signal=" sig; print "bitrate=" rate }
		}'
	ip -4 -o addr show dev $IF 2>/dev/null | awk '{ print "ipv4=" $4 }'
	ip -6 -o addr show dev $IF scope global 2>/dev/null | awk '{ print "ipv6=" $4 }'
	printf 'configured=%s\n' "$(lidar-feature get wifi ssid)"
}

scan() {
	wait_if || return 1
	ip link set $IF up
	# a running wpa_supplicant may hold the scan: then use its last results
	out=$(iw dev $IF scan 2>/dev/null) || out=$(iw dev $IF scan dump 2>/dev/null)
	printf '%s\n' "$out" | awk '
		# per network name: the strongest access point, and every band seen
		function emit(  sec) {
			if (!have || ssid == "") return
			sec = rsn ? (psk && sae ? "wpa2/wpa3" : psk ? "wpa2" : sae ? "wpa3" : "enterprise") : wpa ? "wpa" : "open"
			if (!(ssid in best) || sig > best[ssid]) { best[ssid] = sig; secs[ssid] = sec }
			band[ssid, freq >= 5925 ? "6" : freq >= 4900 ? "5" : "2.4"] = 1
		}
		/^BSS / { emit(); have = 1; ssid = ""; sig = -100; freq = 0; rsn = wpa = psk = sae = 0; ie = "" }
		/^[[:space:]]*freq: / { freq = int($2) }
		/^[[:space:]]*signal: / { sig = $2 + 0 }
		/^[[:space:]]*SSID: / { sub(/^[[:space:]]*SSID: /, ""); ssid = $0 }
		/^[[:space:]]*RSN:/ { rsn = 1; ie = "rsn" }
		/^[[:space:]]*WPA:/ { wpa = 1; ie = "wpa" }
		# RSN key management: PSK is WPA2-Personal, SAE WPA3, 802.1X Enterprise
		/Authentication suites:/ && ie == "rsn" { if (/PSK/) psk = 1; if (/SAE/) sae = 1 }
		END {
			emit()
			for (s in best) {
				b = ""
				if ((s, "2.4") in band) b = "2.4"
				if ((s, "5") in band) b = b (b == "" ? "" : " ") "5"
				if ((s, "6") in band) b = b (b == "" ? "" : " ") "6"
				print best[s] "\t" b "\t" secs[s] "\t" s
			}
		}' | sort -t "$(printf '\t')" -k1,1nr
}

case $1 in
start) start ;;
stop) stop ;;
restart) stop; start ;;
status) status ;;
scan) scan ;;
*)
	sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
