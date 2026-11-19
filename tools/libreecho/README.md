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
| Services | ✅ 15 of 16 start (logd, timed, audiod, micd, waked, ledd, buttond, timerd, mdnsd, airplayd, radiod, ttsd, sttd, agentd, web); esphomed needs a MAC (below) |
| Microphones | ✅ 7 mics at 16 kHz in LibreEcho's format; its wake-word engine runs on them (not yet tried with a spoken wake word) |
| Audio output | ✅ LibreEcho's engine plays into a loopback (verified digitally); 🧪 loopback → speaker forwarder written, not yet listened to (`libreecho speaker`, default off) |
| LED ring | ✅ LibreEcho's frames drive the ring (orientation not yet compared with LibreEcho's pixel 0) |
| Privacy switch | ✅ `amz_privacy` module mirrors the latch where LibreEcho reads it |
| Buttons | 🧪 volume and action keys reach buttond (action is KEY_HELP, as stock); mic button sends KEY_MICMUTE, which LibreEcho treats as a software mute toggle |
| Web control centre | ✅ `http://169.254.7.2:8080/` (first-run setup) |
| Wi-Fi, Bluetooth, Home Assistant | ❌ LibreEcho's networkd/btd drive MT8163 tools; esphomed needs a stable MAC; not tried on the development unit |
| Persistent storage | ❌ the release lives in RAM (`/run/libreecho-addon`) and is lost on reboot; where to keep 250 MB is open |

## Use

On the host, download a release and keep its `SHA256SUMS` (e.g. tag
`radar-puffin-v0.14.6`): `libreecho-<target>-<version>-boot.img` and the five
feature images `-wakeword`, `-stt`, `-tts`, `-assistant`, `-airplay2`
(`.squashfs`). Copy them with the `SHA256SUMS` file into one directory on the
Echo, then:

```sh
lidar-feature enable libreecho     # takes ring, volume, dsp, usbaudio, webui off
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

## How it fits

```
LibreEcho (ARM32, sandbox)               lidar (arm64)
  micd ── tinycap ── /sbin/lidar-capture ── FIFO ── le-adapter ── hw:amazonlidar,1 (16 kHz, 8 ch)
  micd ── tinymix ── /sbin/lidar-mixer: ADC_A-D MICPGA -> ADC1-4 PGA Capture Volume
  engine ── /dev/snd/pcmC0D23p ── snd-aloop ── forwarder ── hw:amazonlidar,0
  ledd ── /sys/bus/i2c/devices/0-003f/frame (bind mount) ── le-adapter ── /sys/class/leds/ringN:*
  buttond ── /sys/devices/platform/amz_privacy/privacy_state ── amz_privacy.ko ── SW_MUTE_DEVICE
```

### Sandbox

LibreEcho runs as root and its tools write boot slots, `misc` and partitions
by MT8163 partition numbers, so it must never reach the eMMC. Two layers:

- **Removed before it runs.** `lidar-libreecho install` deletes, by name
  anywhere in the unpacked tree, LibreEcho's updater, boot control,
  generation/feature transactions, recovery and vendor-import tools, the
  MT8163 Wi-Fi/BT helpers, adbd, ueventd, watchdogd and its PID-1 init.
  Device nodes other than null, zero, full, random, urandom and tty are
  deleted too.
- **Cannot reach a block device.** `le-sandbox` starts the services
  - with every host mount detached (devtmpfs, `/proc`, `/sys`, the system
    partition; what is left of the host is the bare initramfs);
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

With `libreecho speaker on`, the loopback runs on the codec's clock (no drift
between LibreEcho and the speaker); the forwarder primes the codec with a
buffer of silence and then keeps it open, so streams start without clicks.

### Kernel

Needs `CONFIG_SQUASHFS_LZ4` (the feature images), `CONFIG_COMPAT` (ARM32
userspace) and user and PID namespaces.

## Open points

- Storage: about 250 MB for the images plus LibreEcho's `/data`. Options:
  grow the ext4 on `system_b` (1.2 GB partition, 230 MB filesystem today), or
  use `userdata` (Fire OS's data).
- Speaker forwarder: listen test, latency, clicks.
- Speaker tuning: `audio-profile` selects LibreEcho's Echo Dot chain
  (one driver, its loudness EQ and limiter) with the codec as lidar sets it
  (flat, HP driver gain 11); whether that or the Radar chain suits lidar's
  speakers is for a listen test. LibreEcho reads the profile from the
  sandbox's PID-1 root, i.e. this file.
- Mic geometry: lidar's ring mics are mapped to LibreEcho lanes 0-5 in board
  order; the rotation against LibreEcho's 150 + 60n degrees is unverified.
- LED pixel 0 position against LibreEcho's (its idle indicator lights pixels
  10 and 11 as the front centre).
- esphomed (Home Assistant) needs a stable MAC (`--mac` or IDME), and Wi-Fi.
- Upstream: LibreEcho's porting guide asks for a product issue before an
  MT8516 target; with a kernel like this one it could become a real target
  instead of an adapter.
