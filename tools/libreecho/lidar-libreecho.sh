#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# LibreEcho add-on: runs LibreEcho's userspace (a separate project, built for
# the MT8163 Echos, ARM32) on lidar, on top of our kernel. The owner supplies
# an official LibreEcho release; nothing of it is part of this repository.
#
# Safety: LibreEcho runs in a sandbox (le-sandbox) that keeps an unmodified
# release away from the eMMC and from lidar's files in normal operation:
# its own /dev with sound, input and harmless character devices only, every
# host mount but the initramfs detached, and user, mount and PID namespaces
# in which the kernel refuses to create device nodes or mount device
# filesystems. LibreEcho's updater, boot control, recovery and connectivity
# tools are removed from the unpacked tree before it ever runs. Its networkd
# is replaced: le-netd answers its socket from outside, with lidar's Wi-Fi.
#
# It is not a boundary against a deliberately hostile release: a process
# inside holds CAP_SYS_CHROOT in its user namespace and can escape the
# chroot to the initramfs, which stays mounted in its mount namespace (no
# block device node there, but lidar's tools and $LE).
#
# What the sandbox is given to write outside its own tree is $ADAPTER (a
# small tmpfs, made anew at every start) and $LE/data. Nothing the host's
# root writes or bind-mounts by name after the sandbox ran may live there:
# a symlink could have been left in its place. le-netd therefore keeps its
# files in $NETD, of which the sandbox is only given bind mounts (the
# socket, and the DNS configuration read-only).
#
# Usage: lidar-libreecho install <dir>   verify a release in <dir>, unpack it
#                                        (the feature images are moved, not copied)
#        lidar-libreecho start | stop | status
LE=${LIDAR_LIBREECHO_DIR:-/run/libreecho-addon}
ROOT=$LE/root
ADAPTER=$LE/adapter
NETD=$LE/netd
SHARE=/usr/share/lidar/libreecho
FEATURES="wakeword stt tts assistant airplay2"
# Services in LibreEcho's own start order; mdnsd also runs the D-Bus/Avahi
# runtime airplayd needs. networkd is lidar's stand-in (networkd.init, with
# le-netd outside). Left out: btd (it needs LibreEcho's own BT stack),
# watchdogd (it would restart services).
SERVICES="logd timed networkd audiod micd waked ledd buttond timerd mdnsd airplayd radiod ttsd sttd agentd esphomed web"
# Never left in the tree: they write boot slots, misc or partitions, or
# drive MT8163 hardware. Matched by name anywhere below the root.
REMOVE_NAMES="libreecho-update* libreecho-bootctl* libreecho-generation*
	libreecho-feature-transaction* libreecho-vendor-import*
	libreecho-recovery-* wmt_* adbd ueventd watchdogd"
# LibreEcho's PID-1 and its rc files, at the top of the ramdisk
REMOVE_TOP="init init.rc init.recovery.mt8163.rc libreecho-init"
# The only device nodes kept from the ramdisk (major:minor, hex as stat
# prints them): null zero full random urandom tty
NODES_OK="1:3 1:5 1:7 1:8 1:9 5:0"

die() { echo "lidar-libreecho: $*" >&2; exit 1; }

u32() { # file offset -> little-endian u32
	od -An -tu4 -j "$2" -N 4 "$1" | tr -d ' '
}

one() { # the single file matching a pattern, or fail
	set -- $1
	[ $# = 1 ] && [ -f "$1" ] || die "need exactly one $(basename "$1")"
	echo "$1"
}

install_release() {
	src=$1
	[ -d "$src" ] || die "no directory $src"
	status >/dev/null && die "running; stop it first"
	sums=$(one "$src/libreecho-*-SHA256SUMS") || exit 1
	boot=$(one "$src/libreecho-*-boot.img") || exit 1
	files=$boot
	for x in $FEATURES; do
		files="$files $(one "$src/libreecho-*-v*-$x.squashfs")" || exit 1
	done
	# every file used must match the release's checksums
	for f in $files; do
		want=$(awk -v n="$(basename "$f")" '$2 == n || $2 == "*" n { print $1 }' "$sums")
		[ -n "$want" ] && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$want" ] ||
			die "checksum mismatch: $f"
	done
	# Android boot image: the ramdisk follows the kernel, page aligned
	[ "$(head -c 8 "$boot")" = ANDROID! ] || die "not a boot image: $boot"
	ks=$(u32 "$boot" 8); rs=$(u32 "$boot" 16); pg=$(u32 "$boot" 36)
	[ "$pg" -gt 0 ] && [ "$rs" -gt 0 ] || die "bad boot image header"

	rm -rf "$ROOT" "$LE/payloads"
	mkdir -p "$ROOT" "$LE/payloads" "$LE/data"
	skip=$(( (pg + (ks + pg - 1) / pg * pg) / pg ))
	dd if="$boot" bs="$pg" skip="$skip" count=$(( (rs + pg - 1) / pg )) 2>/dev/null |
		head -c "$rs" | xzcat | (cd "$ROOT" && cpio -idm) ||
		die "cannot unpack the ramdisk"
	[ -x "$ROOT/bin/busybox" ] && [ -d "$ROOT/etc/init.d" ] || die "unexpected ramdisk"
	for d in bin sbin etc usr usr/local dev run tmp var data; do
		[ -L "$ROOT/$d" ] && die "unexpected symlink /$d in the ramdisk"
	done
	for n in $REMOVE_NAMES; do
		find "$ROOT" -xdev -name "$n" ! -type d -exec rm -f {} +
	done
	for f in $REMOVE_TOP; do rm -f "$ROOT/$f"; done
	find "$ROOT" -xdev \( -type b -o -type c \) | while read -r n; do
		[ -c "$n" ] && case " $NODES_OK " in *" $(stat -c %t:%T "$n") "*) continue ;; esac
		rm -f "$n"
	done
	# moved, not copied: the images are a quarter of the RAM
	for f in $files; do
		[ "$f" = "$boot" ] && continue
		x=${f##*-}
		mv "$f" "$LE/payloads/${x%.squashfs}.squashfs" || die "cannot store $f"
	done
	# lidar's adapters and settings; networkd is lidar's stand-in, and DNS
	# comes from lidar's DHCP client (le-netd keeps the copy current, in a
	# directory start mounts read-only at /run/lidar-net)
	mkdir -p "$ROOT/adapter" "$ROOT/etc/default" "$ROOT/data" &&
		cp "$SHARE"/audio-profile "$ROOT/etc/libreecho/audio-profile" &&
		cp "$SHARE"/default/* "$ROOT/etc/default/" &&
		install -m755 "$SHARE"/capture "$ROOT/sbin/lidar-capture" &&
		install -m755 "$SHARE"/mixer "$ROOT/sbin/lidar-mixer" &&
		install -m755 "$SHARE"/networkd.init "$ROOT/etc/init.d/libreecho-networkd.init" &&
		rm -f "$ROOT/etc/resolv.conf" &&
		ln -s /run/lidar-net/resolv.conf "$ROOT/etc/resolv.conf" ||
		die "cannot install the lidar settings"
	echo "lidar-libreecho: installed $(basename "$boot" -boot.img) in $LE"
}

dev_of() { # class/name -> major minor
	tr ':' ' ' < "/sys/class/$1/dev"
}

card_of() { # ALSA card id -> number
	c=$(readlink -f "/proc/asound/$1") || return 1
	echo "${c##*card}"
}

# The tree below $ROOT is the sandbox's to write, and the last one may have
# left a symlink anywhere in it. Paths (relative to $ROOT) that root is
# about to create, write or mount on must not be one, nor lead through one:
# it would be followed on the host.
no_links() {
	for _p in "$@"; do
		_q=$ROOT
		for _c in $(echo "$_p" | tr '/' ' '); do
			_q=$_q/$_c
			if [ -L "$_q" ]; then
				die "${_q#"$ROOT"} is a symlink in the LibreEcho tree; install the release again"
			fi
		done
	done
}

# Runs in a private mount namespace: everything mounted here disappears
# with the last LibreEcho process.
start_inner() {
	[ "$LIDAR_LE_INNER" = 1 ] || die "start-inner is internal; use start"
	set -e
	mount --make-rprivate /
	m=usr/local/lib/libreecho-mdns/root/dev
	no_links dev proc sys run tmp var/run var/log data adapter \
		$m/null $m/random $m/urandom
	for d in dev proc sys run tmp var/run var/log data adapter; do
		mkdir -p "$ROOT/$d"
	done
	# /dev: no block devices; read-only once populated
	mount -t tmpfs -o size=1m,mode=0755,nosuid tmpfs "$ROOT/dev"
	for n in null:1:3 zero:1:5 full:1:7 random:1:8 urandom:1:9 tty:5:0; do
		mknod -m666 "$ROOT/dev/${n%%:*}" c $(echo "${n#*:}" | tr ':' ' ')
	done
	mkdir -p "$ROOT/dev/pts" "$ROOT/dev/shm" "$ROOT/dev/snd" "$ROOT/dev/input"
	codec=$(card_of amazonlidar)
	loop=$(card_of Loopback)
	# LibreEcho's PCM numbers: playback 23 goes into the loopback (its
	# capture side is the lidar end), capture 24 only has to exist.
	mknod -m660 "$ROOT/dev/snd/controlC0" c $(dev_of sound/controlC$codec)
	mknod -m660 "$ROOT/dev/snd/pcmC0D23p" c $(dev_of sound/pcmC${loop}D1p)
	mknod -m660 "$ROOT/dev/snd/pcmC0D24c" c $(dev_of sound/pcmC${codec}D1c)
	mknod -m660 "$ROOT/dev/snd/timer" c $(dev_of sound/timer)
	for e in /sys/class/input/event*; do
		grep -qx gpio-keys "$e/device/name" 2>/dev/null &&
			mknod -m660 "$ROOT/dev/input/$(basename "$e")" c $(dev_of "input/$(basename "$e")")
	done
	ln -s pts/ptmx "$ROOT/dev/ptmx"
	# Nodes LibreEcho's mDNS chroot creates itself on its own kernel (the
	# sandbox cannot)
	m=$ROOT/$m
	mkdir -p "$m"
	for n in null:1:3 random:1:8 urandom:1:9; do
		[ -c "$m/${n%%:*}" ] ||
			mknod -m666 "$m/${n%%:*}" c $(echo "${n#*:}" | tr ':' ' ')
	done
	# LibreEcho's scripts log to /dev/kmsg and /dev/console: both are a plain
	# file outside (never the kernel's log)
	: > "$ROOT/dev/kmsg"
	: > "$ROOT/dev/console"
	mount -o remount,ro "$ROOT/dev"
	mount --bind "$ADAPTER/console.log" "$ROOT/dev/kmsg"
	mount --bind "$ADAPTER/console.log" "$ROOT/dev/console"
	mount -t devpts -o newinstance,ptmxmode=0666,nosuid,noexec devpts "$ROOT/dev/pts"
	mount -t tmpfs -o size=16m,mode=1777,nosuid,nodev tmpfs "$ROOT/dev/shm"
	# host view; le-sandbox mounts the namespace's own /proc over it
	mount -t proc proc "$ROOT/proc"
	# read-only, apart from the files LibreEcho writes: the LED frames
	# (mounted over it below) and the privacy module's mute lamp
	mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$ROOT/sys"
	no_links sys/devices/platform/amz_privacy sys/class/leds sys/bus/i2c/devices
	mount --bind /sys/devices/platform/amz_privacy "$ROOT/sys/devices/platform/amz_privacy"
	# LEDs: hide the per-channel class LEDs and offer the IS31 frame files
	mount -t tmpfs -o size=64k,nosuid,nodev tmpfs "$ROOT/sys/class/leds"
	mount --bind "$ADAPTER/i2c" "$ROOT/sys/bus/i2c/devices"
	mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$ROOT/run"
	mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs "$ROOT/tmp"
	mkdir -p "$ROOT/var/run" "$ROOT/var/log"
	for x in $FEATURES; do
		mkdir -p "$ROOT/run/libreecho/features/$x/root"
		mount -t squashfs -o loop,ro,nosuid,nodev,noatime "$LE/payloads/$x.squashfs" \
			"$ROOT/run/libreecho/features/$x/root"
	done
	no_links run/libreecho/network.sock run/lidar-net
	# LibreEcho's network daemon socket: le-netd's, bound over a file so
	# it is a real socket inode there and cannot be removed
	mkdir -p "$ROOT/run/libreecho" "$ROOT/run/lidar-net"
	: > "$ROOT/run/libreecho/network.sock"
	mount --bind "$NETD/network.sock" "$ROOT/run/libreecho/network.sock"
	# le-netd's copy of lidar's DNS configuration (/etc/resolv.conf points
	# here), read-only: le-netd writes there as the host's root
	mount --bind "$NETD/dns" "$ROOT/run/lidar-net"
	mount -o remount,bind,ro,nosuid,nodev,noexec "$ROOT/run/lidar-net"
	# airplayd binds /dev and /sys into its chroot itself; with their own
	# submounts that needs the host side (it skips mounts that exist)
	a=run/libreecho/features/airplay2/root
	no_links $a/dev $a/sys
	a=$ROOT/$a
	mount --bind "$ROOT/dev" "$a/dev"
	mount --rbind "$ROOT/sys" "$a/sys"
	mount --bind "$LE/data" "$ROOT/data"
	mount -o remount,bind,nosuid,nodev "$ROOT/data"
	mount --bind "$ADAPTER" "$ROOT/adapter"
	mount -o remount,bind,nosuid,nodev "$ROOT/adapter"
	exec /usr/bin/le-sandbox "$ROOT" "$LE/sandbox.pid" /bin/sh -c '
		for s in '"$SERVICES"'; do
			/etc/init.d/libreecho-$s.init start > /adapter/start-$s.log 2>&1
			echo "$s $?" >> /adapter/started
		done'
}

# PID 1 of the sandbox (host PID), if it runs
sandbox_pid() {
	p=$(cat "$LE/sandbox.pid" 2>/dev/null) || return 1
	[ -n "$p" ] && [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = /usr/bin/le-sandbox ] &&
		echo "$p"
}

# every process in the sandbox's PID namespace except PID 1
sandbox_members() {
	ns=$(readlink "/proc/$1/ns/pid") || return
	for p in /proc/[0-9]*; do
		[ "${p#/proc/}" != "$1" ] && [ "$(readlink "$p/ns/pid" 2>/dev/null)" = "$ns" ] &&
			echo "${p#/proc/}"
	done
}

start() {
	[ -x "$ROOT/bin/busybox" ] || die "not installed (lidar-libreecho install <dir>)"
	status >/dev/null && die "already running"
	stop
	# The loopback runs on its own timer: it delivers silence while
	# LibreEcho plays nothing, so the forwarder below keeps the codec
	# running (the amplifier unmutes 500 ms after the DAC starts and would
	# cut every sound short otherwise). A loopback left on the codec's clock
	# by an older start is loaded again.
	speaker=$(lidar-feature get libreecho speaker)
	if cur=$(cat /sys/module/snd_aloop/parameters/timer_source 2>/dev/null); then
		cur=${cur%%,*}
		[ "${cur:-(null)}" = '(null)' ] || rmmod snd_aloop || die "snd-aloop busy"
	fi
	modprobe snd-aloop pcm_substreams=1
	[ -e /proc/asound/Loopback ] || die "no snd-aloop"
	[ -d /sys/devices/platform/amz_privacy ] || insmod "$SHARE/amz_privacy.ko" || die "amz_privacy"
	# A fresh adapter directory at every start: the last sandbox could
	# write there and may have left symlinks where files are created or
	# bind-mounted below (console.log, i2c, mic.fifo). It is dead by now,
	# and rm does not follow links. A tmpfs of its own caps what the
	# sandbox can fill (console.log above all); the old one stays readable
	# after stop and goes here.
	while umount -l "$ADAPTER" 2>/dev/null; do :; done
	rm -rf "$ADAPTER" "$NETD" || die "cannot clear $ADAPTER"
	mkdir -p "$ADAPTER" "$NETD/dns" "$LE/data/libreecho"
	mount -t tmpfs -o size=8m,mode=0755,nosuid,nodev tmpfs "$ADAPTER" ||
		die "cannot mount $ADAPTER"
	mkdir -p "$ADAPTER/i2c/0-003f"
	: > "$ADAPTER/console.log"
	# AirPlay's PTP clock (nqptp) binds ports 319 and 320, which need a
	# capability the sandbox has not got in lidar's network namespace: while
	# LibreEcho runs, ports from 319 up need none (everything on lidar runs
	# as root anyway). stop restores the value.
	[ -f "$LE/port_start" ] || cat /proc/sys/net/ipv4/ip_unprivileged_port_start > "$LE/port_start"
	echo 319 > /proc/sys/net/ipv4/ip_unprivileged_port_start
	: > "$ADAPTER/i2c/0-003f/frame"
	echo 1 > "$ADAPTER/i2c/0-003f/boot_animation"
	setsid /usr/bin/le-adapter "$ADAPTER/i2c/0-003f" "$ADAPTER/mic.fifo" \
		</dev/null >>"$LE/adapter.log" 2>&1 &
	echo $! > "$LE/adapter.pid"
	# LibreEcho's network daemon, answered with lidar's Wi-Fi. Its
	# directory is not the sandbox's to write (the socket was removed with
	# it above, so one that exists is this le-netd's).
	setsid /usr/bin/le-netd "$NETD" </dev/null >>"$LE/netd.log" 2>&1 &
	echo $! > "$LE/netd.pid"
	for i in 1 2 3 4 5 6 7 8 9 10; do
		[ -S "$NETD/network.sock" ] && break
		sleep 0.2
	done
	[ -S "$NETD/network.sock" ] || { stop; die "le-netd did not start (see $LE/netd.log)"; }
	# Speaker: LibreEcho's output (loopback device 1) to the codec, all the
	# time. alsaloop -a 2 starts at once rather than with LibreEcho's
	# streams, and -S 2 corrects the drift between the loopback's timer and
	# the codec's clock through the loopback's PCM Rate Shift.
	if [ "$speaker" = on ]; then
		setsid alsaloop -C hw:Loopback,0,0 -P hw:amazonlidar,0 -f S32_LE -c 2 \
			-r 48000 -t 50000 -S 2 -a 2 </dev/null >>"$LE/speaker.log" 2>&1 &
		echo $! > "$LE/speaker.pid"
	fi
	# The sandbox's PID 1 lives on with these descriptors: never the caller's.
	LIDAR_LE_INNER=1 unshare -m sh "$0" start-inner </dev/null >"$LE/start.log" 2>&1 ||
		{ stop; die "start failed (see $LE/start.log)"; }
	cat "$ADAPTER/started"
}

stop() {
	if p=$(sandbox_pid); then
		m=$(sandbox_members "$p")
		[ -n "$m" ] && kill $m 2>/dev/null
		sleep 2
		kill -9 "$p" 2>/dev/null # ends the whole namespace
		# PID 1 is gone only once every process in the namespace is:
		# what start does next relies on that
		i=0
		while kill -0 "$p" 2>/dev/null; do
			i=$((i + 1))
			if [ "$i" -gt 50 ]; then
				echo "lidar-libreecho: the sandbox is still ending" >&2
				break
			fi
			sleep 0.1
		done
	fi
	rm -f "$LE/sandbox.pid"
	for f in adapter speaker netd; do
		p=$(cat "$LE/$f.pid" 2>/dev/null) &&
			kill -- -"$p" "$p" 2>/dev/null
		rm -f "$LE/$f.pid"
	done
	for l in /sys/class/leds/ring*/brightness; do echo 0 > "$l"; done 2>/dev/null
	if [ -f "$LE/port_start" ]; then
		cat "$LE/port_start" > /proc/sys/net/ipv4/ip_unprivileged_port_start
		rm -f "$LE/port_start"
	fi
	return 0
}

status() {
	p=$(sandbox_pid) || { echo "libreecho: not running"; return 1; }
	echo "libreecho: $(sandbox_members "$p" | wc -l) processes"
}

case $1 in
install) install_release "$2" ;;
start) start ;;
start-inner) start_inner ;;
stop) stop ;;
status) status ;;
*) sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
