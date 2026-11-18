#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Zigbee coordinator: the EFR32 radio running Silicon Labs' EmberZNet NCP,
# reachable over the network for Home Assistant (ZHA) as an EZSP coordinator
# at socket://<echo>:6638.
#
# The EFR32 talks to the SoC on ttyS2. Its Gecko bootloader starts the
# application only while EFR32_SWCLK is high (low = stay in the bootloader,
# where the board holds it at power-on). The application image is the
# owner's own copy of Fire OS's EmberZNet NCP (xncp-fw.gbl on system_a); it
# is uploaded through the bootloader only if the application area holds
# nothing that starts. After a reboot without a power cut the NCP simply
# keeps running.
#
# Usage: lidar-zigbee daemon   bring the NCP up, then bridge it to TCP
#        lidar-zigbee status
TTY=/dev/ttyS2
PORT=6638
RUN=/run/lidar-zigbee
GBL=system/vendor/firmware/silabs/xncp-fw.gbl
ASH_RST='\032\300\070\274\176'	# ASH cancel + RST frame; the NCP answers RSTACK

log() { echo "lidar-zigbee: $*" > /dev/kmsg; }

# Sends $1 (printf format) on fd 3 (the open tty) and prints the reply of
# the next $2 seconds as hex bytes separated by spaces.
exchange() {
	timeout "$2" cat <&3 > $RUN.rx &
	r=$!
	printf "$1" >&3
	wait $r
	echo " $(od -An -tx1 -v $RUN.rx | tr -s ' \n' '  ') "
}

app_running() {
	case $(exchange "$ASH_RST" 2) in *" c1 02 "*) return 0 ;; esac	# RSTACK
	return 1
}

in_bootloader() {
	case $(exchange '\r' 1) in *" 42 4c 20 3e "*) return 0 ;; esac	# "BL >"
	return 1
}

# The bootloader starts the application only while SWCLK is high; keep
# holding it so that an NCP reset comes back into the application.
hold_swclk() {
	pgrep -f 'gpioset -z EFR32_SWCLK' >/dev/null && return
	gpioset -z EFR32_SWCLK=1
}

upload() {
	dev=/dev/disk/by-partlabel/system_a
	mkdir -p $RUN.fireos
	mount -o ro -t ext4 $dev $RUN.fireos || return 1
	ok=1
	if [ -r "$RUN.fireos/$GBL" ] && in_bootloader; then
		printf 1 >&3
		lidar-xmodem $TTY "$RUN.fireos/$GBL" && ok=0
	fi
	umount $RUN.fireos
	return $ok
}

bring_up() {
	stty -F $TTY 115200 raw -echo -crtscts -ixon -ixoff
	hold_swclk || { log "cannot drive EFR32_SWCLK"; return 1; }
	exec 3<> $TTY
	app_running && return 0
	if ! in_bootloader; then
		log "EFR32 answers neither as NCP nor as bootloader"
		return 1
	fi
	printf 2 >&3		# run
	sleep 1
	app_running && { log "NCP started from the bootloader"; return 0; }
	log "no application in the EFR32, uploading the NCP image from system_a"
	if ! upload; then
		log "upload failed"
		return 1
	fi
	sleep 1
	in_bootloader && printf 2 >&3
	sleep 1
	app_running && { log "NCP running"; return 0; }
	log "the uploaded NCP does not start"
	return 1
}

case $1 in
daemon)
	if ! bring_up; then
		sleep 60	# init respawns the daemon; do not hammer the radio
		exit 1
	fi
	exec 3>&-
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
	# One client at a time: the NCP is one serial line. When the client
	# leaves, socat exits and init starts the daemon again.
	exec socat TCP-LISTEN:$PORT,reuseaddr$bind $TTY,raw,echo=0,b115200
	;;
status)
	if pgrep -f "socat TCP-LISTEN:$PORT" >/dev/null; then
		echo "bridge: listening on port $PORT"
	else
		echo "bridge: not running"
	fi
	;;
*)
	sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
