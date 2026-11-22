#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System clock. The board has no real-time clock (no PMIC on it, as on
# stock), so the clock starts at 1970 on every boot. lidar-time sets it from
# the network (BusyBox ntpd, pool.ntp.org) once there is a route and a DNS
# server, keeps it in sync, and saves it on the booted slot at every sync:
# the next boot starts at the last saved time instead of 1970 until NTP
# answers again.
#
# Usage: lidar-time restore   the last saved time, if later than the clock
#                             (init, before the services)
#        lidar-time daemon    ntpd once the network is up (init)
#        lidar-time save      save the time now
# ntpd runs this script with step|stratum|periodic|unsync (-S). Once NTP has
# answered, /run/lidar-time.synced exists: the clock is right, not just the
# last saved time (alarms wait for it).
DIR=/var/lib/lidar-time
SERVER=pool.ntp.org

log() { echo "lidar-time: $*" > /dev/kmsg; }

save() {
	lidar-feature keep $DIR >/dev/null 2>&1 || return 1
	date +%s > $DIR/last.$$ && mv $DIR/last.$$ $DIR/last
}

case $1 in
restore)
	lidar-feature keep $DIR >/dev/null 2>&1 || exit 0
	t=$(cat $DIR/last 2>/dev/null)
	case $t in '' | *[!0-9]*) exit 0 ;; esac
	if [ "$t" -gt "$(date +%s)" ] && date -s "@$t" >/dev/null; then
		log "clock set to the last saved time, $(date)"
	fi
	;;
daemon)
	# a default route and a DNS server: the network is up (Wi-Fi's DHCP)
	until ip route | grep -q '^default' && grep -q '^nameserver' /etc/resolv.conf 2>/dev/null; do
		sleep 5
	done
	ntpd -n -S /usr/bin/lidar-time -p $SERVER 2>&1 | while IFS= read -r l; do
		[ -z "$l" ] || printf 'ntpd: %s\n' "$l" > /dev/kmsg
	done
	sleep 30	# ntpd ended (e.g. no DNS answer): init starts it again
	;;
step)
	: > /run/lidar-time.synced
	save
	log "clock set from NTP, $(date)"
	;;
stratum | periodic)
	: > /run/lidar-time.synced
	save
	;;
save) save ;;
unsync) ;;
*)
	sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
