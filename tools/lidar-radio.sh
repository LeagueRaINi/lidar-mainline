#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The EFR32 radio for Zigbee and Thread at the same time: this project's
# multi-PAN RCP (efr32/ in the source tree) serves both networks on one
# channel, cpcd multiplexes its serial line, zigbeed runs the Zigbee stack
# and otbr-agent the Thread border router on the Echo.
#
# - zigbee: zigbeed's EZSP port, bridged to TCP 6638 for Home Assistant
#   (ZHA, socket://<echo>:6638), one client at a time
# - thread: otbr-agent with its REST API on port 8081 for Home Assistant's
#   OpenThread Border Router integration; ot-ctl is its command line here
#
# The RCP image ships in this system; it is uploaded through the EFR32's
# bootloader (lidar-efr32) only if cpcd finds no RCP answering. The Zigbee
# and Thread networks live in /var/lib/zigbee and /var/lib/thread, bound
# from the same directories on the booted slot: the slot's filesystem is
# made writable underneath while its mount at /media/system stays read-only.
#
# Usage: lidar-radio cpcd     bring the RCP up and run cpcd
#        lidar-radio zigbee   run zigbeed and its TCP bridge
#        lidar-radio thread   run otbr-agent
#        lidar-radio status
GBL=/usr/share/lidar/efr32/lidar-mp-rcp.gbl
RUN=/run/lidar-radio
UP=$RUN.cpcd-up
PORT=6638
PTY=/run/zigbeed.pty

log() { echo "lidar-radio: $*" > /dev/kmsg; }

# Forwards a daemon's output to the kernel log line by line, as "<name>: ".
# Writes in pieces would otherwise each become a record of their own.
to_kmsg() {
	while IFS= read -r l; do
		[ -z "$l" ] || printf '%s: %s\n' "$1" "$l" > /dev/kmsg
	done
}

# The networks' data directory, bound from the booted slot
keep_data() {
	e=$(lidar-feature keep "$1" 2>&1) || { log "$1: ${e:-cannot bind it}"; return 1; }
}

# cpcd in the background; true once it reports the RCP connected. The log
# reader lives as long as cpcd holds the pipe, and exists from the start.
start_cpcd() {
	rm -f $UP
	cpcd -c /etc/cpcd.conf 2>&1 | while IFS= read -r l; do
		case $l in *"Daemon startup was successful"*) : > $UP ;; esac
		printf 'cpcd: %s\n' "$l" > /dev/kmsg
	done &
	r=$!
	for i in $(seq 1 100); do
		[ -e $UP ] && return 0
		kill -0 $r 2>/dev/null || return 1
		sleep 0.2
	done
	return 1
}

stop_cpcd() {
	pkill -x cpcd
	while pgrep -x cpcd >/dev/null; do sleep 0.1; done
	rm -f $UP
}

wait_cpcd() {
	for i in $(seq 1 60); do
		[ -e $UP ] && return 0
		sleep 1
	done
	log "$1: cpcd is not up"
	return 1
}

case $1 in
cpcd)
	# libcpc's sockets live in /dev/shm
	mountpoint -q /dev/shm || { mkdir -p /dev/shm && mount -t tmpfs -o mode=1777 shm /dev/shm; }
	stop_cpcd
	lidar-efr32 run || { log "cannot start the EFR32 application"; sleep 60; exit 1; }
	if ! start_cpcd; then
		stop_cpcd
		# once per boot: a failure on this side (configuration, UART) is
		# not cured by uploading the image again and again
		if [ -e $RUN.flashed ]; then
			log "cpcd does not connect to the RCP"
			sleep 60
			exit 1
		fi
		: > $RUN.flashed
		log "no multi-PAN RCP answers, uploading ${GBL##*/}"
		if ! lidar-efr32 flash $GBL || ! start_cpcd; then
			stop_cpcd
			log "the RCP does not start"
			sleep 60	# init respawns the daemon; do not hammer the radio
			exit 1
		fi
	fi
	log "cpcd up"
	wait
	rm -f $UP
	;;
zigbee)
	wait_cpcd zigbee || { sleep 10; exit 1; }
	command -v zigbeed >/dev/null || { log "zigbeed is not in this image"; sleep 60; exit 1; }
	# usb: the socket only takes connections that arrive on usb0
	# (SO_BINDTODEVICE), whatever address they are sent to.
	bind=,so-bindtodevice=usb0
	if [ "$(lidar-feature get zigbee listen)" = any ]; then
		bind=
	elif [ ! -d /sys/class/net/usb0 ]; then
		log "no usb0 to listen on"
		sleep 60
		exit 1
	fi
	keep_data /var/lib/zigbee || { sleep 60; exit 1; }
	pkill -x zigbeed
	# zigbeed opens an existing tty: socat creates it, then waits for the
	# client. Both go together: when the client leaves socat ends, zigbeed
	# with it, and init starts the pair again.
	rm -f $PTY
	socat PTY,link=$PTY,raw,echo=0 TCP-LISTEN:$PORT,reuseaddr$bind &
	sp=$!
	for i in $(seq 1 50); do
		[ -e $PTY ] && break
		sleep 0.1
	done
	# tokens (the network) go to host_token.nvm in the working directory;
	# -v: its log to stderr (there is no syslogd), -d 2: warnings and up
	(cd /var/lib/zigbee &&
	 exec zigbeed -r 'spinel+cpc://cpcd_0?iid=1&iid-list=0' -p $PTY -d 2 -v) 2>&1 | to_kmsg zigbeed &
	sleep 2
	while kill -0 $sp 2>/dev/null && pgrep -x zigbeed >/dev/null; do
		sleep 2
	done
	kill $sp 2>/dev/null
	pkill -x zigbeed
	wait
	;;
thread)
	# The agent outlives this script if only the script is killed; end it
	# before a new one takes the radio
	if pkill -x otbr-agent; then
		for i in $(seq 1 50); do
			pgrep -x otbr-agent >/dev/null || break
			sleep 0.1
		done
	fi
	infra=$(lidar-feature get thread infra)
	# usb: a unix socket, which init's lidar-listen bridges to port 8081 for
	# connections arriving on usb0 only (an address alone would also be
	# reachable through wlan0)
	rest=/run/otbr-rest.sock
	[ "$(lidar-feature get thread listen)" = any ] && rest=::
	# Wi-Fi comes up in the background after boot
	for i in $(seq 1 60); do
		[ -d /sys/class/net/$infra ] && break
		sleep 1
	done
	if [ ! -d /sys/class/net/$infra ] || ! wait_cpcd thread || ! keep_data /var/lib/thread; then
		[ -d /sys/class/net/$infra ] || log "no $infra"
		sleep 60
		exit 1
	fi
	# Border routing: forward IPv6, and keep taking router advertisements
	# (with route information) on the infrastructure link while forwarding.
	echo 1 > /proc/sys/net/ipv6/conf/all/forwarding
	echo 2 > /proc/sys/net/ipv6/conf/$infra/accept_ra
	echo 64 > /proc/sys/net/ipv6/conf/$infra/accept_ra_rt_info_max_plen
	otbr-agent -I wpan0 -B "$infra" --data-path /var/lib/thread -s -d 5 \
		--rest-listen-address $rest 'spinel+cpc://cpcd_0?iid=2&iid-list=0' 2>&1 |
		to_kmsg otbr-agent
	;;
status)
	if [ -e $UP ]; then echo "cpcd: up"; else echo "cpcd: not up"; fi
	if pgrep -x zigbeed >/dev/null; then
		echo "zigbeed: running, bridge on port $PORT"
	else
		echo "zigbeed: not running"
	fi
	if pgrep -x otbr-agent >/dev/null; then
		echo "otbr-agent: running, Thread $(ot-ctl state | head -n1)"
	else
		echo "otbr-agent: not running"
	fi
	;;
*)
	sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
