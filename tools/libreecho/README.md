# LibreEcho add-on (experimental)

Runs the userspace of [LibreEcho](https://github.com/aslater3/LibreEcho), a
separate project (MIT; its wake-word model is CC-BY-NC-SA-4.0), on lidar:
local wake word, speech recognition and synthesis, timers, radio, AirPlay,
its web control centre and Home Assistant integration. LibreEcho itself
targets the MT8163 Echos (Echo 2nd gen, Echo Dot 2nd gen) and ships ARM32
binaries for its own kernel; this add-on runs an unmodified official release
in a sandbox on lidar's arm64 kernel and adapts the hardware interfaces it
expects. Nothing of LibreEcho is part of this repository: the owner downloads
a release and installs it.

## Status

| Part | State |
|---|---|
| Services | ✅ 16 of 17 start (logd, timed, networkd (lidar's stand-in), audiod, micd, waked, ledd, buttond, timerd, mdnsd, airplayd, radiod, ttsd, sttd, agentd, web); esphomed needs a MAC (below) |
| Microphones | ✅ 7 mics at 16 kHz in LibreEcho's format; its wake-word engine runs on them (not yet tried with a spoken wake word) |
| Audio output | ✅ LibreEcho's engine plays into a loopback, `alsaloop` forwards it to the speaker (`libreecho speaker on`, default off); listened to: AirPlay from an iPhone plays cleanly, LibreEcho's volume control works |
| LED ring | ✅ LibreEcho's frames drive the ring (orientation not yet compared with LibreEcho's pixel 0) |
| Privacy switch | ✅ `amz_privacy` module mirrors the latch where LibreEcho reads it |
| Buttons | 🧪 volume and action keys reach buttond (action is KEY_HELP, as stock); mic button sends KEY_MICMUTE, which LibreEcho treats as a software mute toggle |
| Web control centre | ✅ serves `http://169.254.7.2:8080/`; its first-run setup completes (account, hostname, wake word, Wi-Fi, features) |
| Wi-Fi | ✅ through lidar's Wi-Fi: `le-netd` answers LibreEcho's network daemon socket (status, scan, joining a network into lidar's `wifi` settings); open networks and LibreEcho's recovery access point are not supported |
| AirPlay | ✅ announced over mDNS (Avahi) and seen by an iPhone; plays to the speaker |
| Bluetooth, Home Assistant | ❌ LibreEcho's btd drives MT8163 tools and is left out (lidar's `bluetooth` service is off while LibreEcho runs); esphomed needs a stable MAC |
| Persistent storage | ❌ the release lives in RAM (`/run/libreecho-addon`) and is lost on reboot, setup included; where to keep 250 MB is open |

## Use

On the host, download a release and keep its `SHA256SUMS` (e.g. tag
`radar-puffin-v0.14.6`): `libreecho-<target>-<version>-boot.img` and the five
feature images `-wakeword`, `-stt`, `-tts`, `-assistant`, `-airplay2`
(`.squashfs`). Copy them with the `SHA256SUMS` file into one directory on the
Echo, then:

```sh
lidar-feature enable libreecho     # takes ring, volume, dsp, bluetooth, visualizer, usbaudio, webui, thread off
reboot
lidar-libreecho install <dir>      # verifies, unpacks; moves the images
lidar-libreecho start              # status | stop
```

`lidar-feature set libreecho speaker on` routes LibreEcho to the speaker
from the next start. `LE_LED_SCALE=<percent> lidar-libreecho start` dims the
ring (manual starts only; at boot it runs at full brightness).

LibreEcho's services listen on every network interface (web control centre
on 8080 with a first-run setup, AirPlay, mDNS, ESPHome API). Over USB that
is only the host; with Wi-Fi up they are reachable from that network too.

While it runs, ports from 319 up need no capability
(`net.ipv4.ip_unprivileged_port_start`, restored by `stop`): AirPlay's PTP
clock (`nqptp`) binds 319 and 320, and the sandbox has no capabilities in
lidar's network namespace. lidar's `thread` service stays off with LibreEcho:
its `otbr-agent` answers mDNS on port 5353 itself, where LibreEcho runs Avahi.

## How it fits

```
LibreEcho (ARM32, sandbox)               lidar (arm64)
  micd ── tinycap ── /sbin/lidar-capture ── FIFO ── le-adapter ── hw:amazonlidar,1 (16 kHz, 8 ch)
  micd ── tinymix ── /sbin/lidar-mixer: ADC_A-D MICPGA -> ADC1-4 PGA Capture Volume
  engine ── /dev/snd/pcmC0D23p ── snd-aloop ── alsaloop ── hw:amazonlidar,0
  web ── /run/libreecho/network.sock (bind mount) ── le-netd ── lidar-wifi, lidar-feature
  ledd ── /sys/bus/i2c/devices/0-003f/frame (bind mount) ── le-adapter ── /sys/class/leds/ringN:*
  buttond ── /sys/devices/platform/amz_privacy/privacy_state ── amz_privacy.ko ── SW_MUTE_DEVICE
```

### Sandbox

LibreEcho runs as root and its tools write boot slots, `misc` and partitions
by MT8163 partition numbers, so it must never reach the eMMC. The sandbox
keeps an unmodified release away from the eMMC and from lidar's files in
normal operation. It is **not a boundary against a deliberately hostile
release**: the initramfs stays mounted in the sandbox's mount namespace,
hidden only by a chroot, and a process inside holds `CAP_SYS_CHROOT` in its
user namespace (LibreEcho's own mDNS and AirPlay chroots need it), so it can
escape the chroot and write lidar's files in the initramfs. It finds no
block device node there and cannot make one, but it could change a lidar
tool that host root runs later, and through that reach everything. Two
layers:

- **Removed before it runs.** `lidar-libreecho install` deletes, by name
  anywhere in the unpacked tree, LibreEcho's updater, boot control,
  generation/feature transactions, recovery and vendor-import tools, the
  MT8163 Wi-Fi/BT helpers, adbd, ueventd, watchdogd and its PID-1 init.
  Device nodes other than null, zero, full, random, urandom and tty are
  deleted too.
- **Cannot reach a block device.** `le-sandbox` starts the services
  - with every host mount detached (devtmpfs, `/proc`, `/sys`, the system
    partition; what is left of the host is the bare initramfs, behind the
    chroot);
  - in a user namespace with an identity uid map: file permissions as root,
    but capabilities only for the namespace's own objects, so the kernel
    refuses `mknod`, devtmpfs and other device filesystems, module loading
    and raw I/O, and the prepared mounts are locked;
  - in a PID namespace with its own `/proc`, so no host process (and no
    host root through `/proc/<pid>/root`) is visible;
  - with its own `/dev` (read-only; sound, gpio-keys input, null and the
    like) and no block devices anywhere.

  Checked from inside, as root: mounting devtmpfs, `mknod` of the eMMC's
  block device, remounting `/dev` writable all fail; `/proc` lists only
  LibreEcho; the mount table has no ext4, devtmpfs or debugfs.

`stop` ends the namespace's PID 1, which ends every LibreEcho process. The
mounts LibreEcho's own scripts cannot make in a user namespace (non-recursive
binds of `/dev` and `/sys` into the AirPlay chroot, the device nodes of the
mDNS chroot) are prepared outside.

### Audio controls

LibreEcho's engine sets controls of its MT8163 kernel around every output
start (`MFP Gpio Mute`, `Ext_Speaker_Amp_Switch`, `Audio_DacMux_Setting`,
`Board Channel Config`, `Right Channel Only`, `Speaker Codec Profile`) and
refuses to play without them. lidar's machine driver powers and mutes its
amplifier itself, so `le-adapter` adds them as user-defined placeholder
controls. LibreEcho's audiod keeps `PCM Playback Volume` at 127 (0 dB, the
stock maximum) and the engine applies the volume digitally.

With `libreecho speaker on`, `alsaloop` forwards the loopback to the codec
all the time (`-a 2`: from the start, not per stream). The loopback runs on
its own timer and delivers silence while LibreEcho plays nothing, so the
codec stays open and sounds start whole (the amplifier unmutes 500 ms after
the DAC starts). `alsaloop -S 2` corrects the drift between the loopback's
timer and the codec's clock through the loopback's PCM Rate Shift.

### Network

LibreEcho's own networkd runs wpa_supplicant and DHCP itself, which the
sandbox cannot (no capabilities in lidar's network namespace). `le-netd`
answers its socket instead, from outside, with lidar's Wi-Fi: `status` from
`lidar-wifi status` and the default route, `scan` from `lidar-wifi scan`,
`connect` saves the network in lidar's `wifi ssid`/`passphrase` and joins
it, or accepts the network lidar is already on as it is; a network that is
not joined within 90 s is undone. Inside, a stand-in `networkd.init` keeps
the pid file LibreEcho's web service checks, and `/etc/resolv.conf` follows
lidar's. The sandbox has its own UTS namespace, so LibreEcho sets its own
hostname.

### What the sandbox can write outside

Two directories of the host are given to the sandbox writable: LibreEcho's
`/data` (`data/`) and `/adapter` (`adapter/`: the LED frame files, the
microphone FIFO, the start logs and `console.log`, the file LibreEcho's
scripts get as `/dev/kmsg` and `/dev/console`). Whatever is in them can be
a symlink the sandbox made, and the host's root would follow it. So:

- `adapter/` is a tmpfs of its own, removed and made anew at every
  `start`, before anything in it is created or bind-mounted. The last one
  stays readable after `stop`. Its 8 MB are shared by everything in it:
  `console.log` (truncated only at `start`) cannot grow beyond that, but
  once it has filled them, the LED frame files and logs cannot grow either.
- `le-netd` runs as root outside and writes nothing there. Its directory
  (`netd/`: the socket, the connect lock, `dns/resolv.conf`) is not among
  the sandbox's mounts; the sandbox gets the socket as a bind mount at
  `/run/libreecho/network.sock` and `netd/dns` read-only at
  `/run/lidar-net`, where its `/etc/resolv.conf` points.
- LibreEcho's own tree (`root/`) is prepared by root before every start
  (mounts, device nodes of the mDNS chroot). `start` refuses to run if a
  path it creates, writes or mounts on there is a symlink or leads through
  one; installing the release again gives a clean tree.

All of this holds for a release that stays inside its chroot (see Sandbox
above): one that escapes it reaches these directories in the initramfs.

### Files

| File | What |
|---|---|
| `lidar-libreecho.sh` | `lidar-libreecho`: install, start, stop, status |
| `le-sandbox.c` | the namespaces LibreEcho's services run in |
| `le-adapter.c` | LED frames to the ring, microphones into the FIFO, placeholder mixer controls |
| `le-netd.c` | LibreEcho's network daemon socket, answered with lidar's Wi-Fi |
| `networkd.init` | stand-in for LibreEcho's networkd service inside the sandbox |
| `capture`, `mixer` | micd's capture and mixer programs inside (`/sbin/lidar-capture`, `/sbin/lidar-mixer`) |
| `audio-profile`, `default/` | LibreEcho's audio profile and service defaults installed into the tree |
| `amz_privacy/` | kernel module: the privacy latch where LibreEcho reads it |

### Kernel

Needs `CONFIG_SQUASHFS_LZ4` (the feature images), `CONFIG_COMPAT` (ARM32
userspace) and user and PID namespaces.

## Open points

- Sandbox: make LibreEcho's tree the real root of the sandbox's mount
  namespace (move + `pivot_root`) so the initramfs is unreachable; until
  then a chroot escape reaches lidar's files in the initramfs.
- Storage: about 250 MB for the images plus LibreEcho's `/data`. Options:
  grow the ext4 on `system_b` (1.2 GB partition, 230 MB filesystem today), or
  use `userdata` (Fire OS's data).
- Bluetooth: LibreEcho's btd is left out; its bluetooth socket could be
  answered from lidar's BlueZ as le-netd does for Wi-Fi.
- Speaker tuning: `audio-profile` selects LibreEcho's Echo Dot chain
  (one driver, its loudness EQ and limiter) with the codec as lidar sets it
  (flat, HP driver gain 11); whether that or the Radar chain suits lidar's
  speakers is for a listen test. LibreEcho reads the profile from the
  sandbox's PID-1 root, i.e. this file.
- Mic geometry: lidar's ring mics are mapped to LibreEcho lanes 0-5 in board
  order; the rotation against LibreEcho's 150 + 60n degrees is unverified.
- LED pixel 0 position against LibreEcho's (its idle indicator lights pixels
  10 and 11 as the front centre).
- esphomed (Home Assistant) needs a stable MAC (`--mac` or IDME).
- Upstream: LibreEcho's porting guide asks for a product issue before an
  MT8516 target; with a kernel like this one it could become a real target
  instead of an adapter.
