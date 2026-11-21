# lidar-mainline: U-Boot and mainline Linux for the Echo Plus 2nd gen

Mainline Linux v7.3 and U-Boot v2026.10 for the Amazon Echo Plus 2nd gen
(codename "lidar", MediaTek MT8516). It covers the bootloader, the kernel, and
the driver work that runs **after** the unlock. There is no exploit or unlock
code here: the device must already run Kaeru LK, as installed by
[`davidwatters/echo-plus-2-unlock`](https://github.com/davidwatters/echo-plus-2-unlock).

## Roadmap: compared with stock

✅ working · ⚠️ works, known issue · 🧪 implemented, needs testing on
hardware · 🛠️ planned · ➖ not planned. Details are under [Status](#status).

| Feature | Stock Fire OS | lidar-mainline |
|---|---|---|
| **Boot and system** | | |
| Bootloader | Amazon LK | ✅ U-Boot as a second stage after LK, USB fastboot, no UART input needed |
| Updates with automatic fallback | ✅ Amazon OTA | ✅ A/B slots with bootcount fallback (verified on hardware) |
| Recovery | ✅ | ✅ Kaeru fastboot (Action) and U-Boot fastboot (Volume Up) |
| Previous boot's log after a crash | ✅ ram_console | ✅ pstore console and panic logs |
| eMMC | ✅ | ✅ HS200 |
| CPU frequency scaling | ✅ | ✅ 598 MHz to 1.3 GHz with voltage scaling |
| CPU idle | WFI with several cores online | ✅ WFI, same as stock |
| Deep idle / suspend | ✅ via SPM firmware | ➖ |
| **Audio** | | |
| Speaker | ✅ | ✅ |
| 3.5 mm line-out | ✅ | ⚠️ works; start-of-stream crack not re-tested since the jack moved to the headphone drivers |
| Speaker crossover (woofer + tweeter) | ✅ | ✅ Amazon's filters in the DAC, amplifier mute against pops |
| Microphones (7) | ✅ | ⚠️ all 7 capture; mic 7 picks up narrow interference tones |
| Echo-cancellation reference | ✅ | ✅ playback loopback capture, bit-exact; used by `lidar-aec` |
| Line-in | capture path in Fire OS's mixer config; use unclear | 🧪 not tested with a real source |
| **Wireless** | | |
| Wi-Fi (MT7668) | ✅ | ✅ scans 2.4 and 5 GHz, connects (tested on 5 GHz: 866 Mbit/s link); firmware from the unit's own Fire OS, kept on lidar's slot (`lidar-fireos`), MAC from IDME |
| Bluetooth (MT7668) | ✅ | ✅ classic and LE scanning, alongside Wi-Fi; ROM patch from linux-firmware. Bluetooth speaker for phones (A2DP, SBC and AAC) with the phone's volume as the system volume, paired by holding the action button (`bluetooth` service) |
| Zigbee hub (EFR32) | ✅ Amazon firmware | ✅ Zigbee coordinator next to Thread on one radio: `zigbeed` (EmberZNet 7.4.3) on the Echo, EZSP over TCP for Home Assistant (`zigbee` service) |
| Thread border router (EFR32) | ✅ | ✅ next to Zigbee on one radio: multi-PAN RCP built from this repository (`efr32/`), `otbr-agent` with its REST API for Home Assistant (`thread` service). Fire OS's own dual-PAN image speaks DSR's multiplexing and is not used |
| **Interface and sensors** | | |
| LED ring (12 RGB) | ✅ | ✅ boot animation; red while the mics are muted, volume arc (`ring` service) |
| Buttons, mic mute, privacy latch | ✅ | ✅ |
| Ambient light sensor | ✅ | ✅ relative readings (no lux calibration) |
| Temperature sensors | ✅ | ✅ |
| **Software** | | |
| Voice assistant | ✅ Alexa | 🛠️ local userspace, e.g. a Home Assistant voice satellite |
| USB speakerphone for a PC | ➖ | ✅ USB Audio Class 2: speaker and 6-mic mono capture with echo cancellation |
| Services, with settings | n/a | ✅ `lidar-feature`: core services on, extras off by default |
| Alexa cloud services | ✅ | ➖ |
| Kernel and U-Boot upstreaming | n/a | 🛠️ patch series drafted |

## Prerequisites

- An Echo Plus 2nd gen that is already unlocked and runs Kaeru LK.
- Your own full eMMC dump from the unlock, at least `boot_a`, for recovery.
- `fastboot` (Android platform-tools) and `podman` on the host.
- Network access while building: the test image downloads Alpine packages.
- Access to the USB test points (TM4, TM5 and TM37 below), for example with
  pogo pins; `pogo-jig/` is a printable jig for that. All flashing, the
  shell and the network go over this USB link.
- Optional: a 1.8 V UART adapter on TM58 (UART0 TX, 921600 8N1), output only.

### Test points

The USB and supply pads form a 2x3 block next to the AMP flex cable:

![Test pad block](docs/test-points.svg)

| Pad | Signal | Notes |
|---|---|---|
| TM4 | USB D+ | device-mode USB to the host |
| TM5 | USB D- | |
| TM37 | GND | |
| TM58 | UART0 TX | 1.8 V, 921600 8N1, kernel and bootloader log |
| TM33 | VIN, about 17 V (adapter rail) | next to USB D+/D- and GND: never connect logic-level tools, and avoid bridging it to neighbouring pads |
| TM45 | USB ID | SoC pin 41 (KPROW1, function `IDDIG`), 1.8 V logic; follows the pin's internal pull (1.8 V with pull-up, 0 V with pull-down). Pulled up by default (device role); leave unconnected |
| TM35 | probably USB VBUS from the factory fixture (unused) | near 0 V. Not a SoC GPIO: no response to pull changes on any free GPIO, and not UART0 RX. Unpowered: open to GND with the pad positive (even on 2 MΩ), ~600 Ω and a 0.61 V diode drop with GND positive, i.e. only a diode to ground. 5 V through 10 kΩ changed nothing the SoC can see (GPIO inputs, USB PHY monitor, MUSB DEVCTL); the board has no MT6392 PMIC to sense VBUS. Leave unconnected |

UART0 RX has not been found yet: none of the pads identified so far carries
it (TM35 was checked). No UART input is needed, see "U-Boot behaviour" below.

## Boot chain

```
BootROM -> preloader (BOOT0) -> BL31 -> Amazon LK + Kaeru
        -> boot_a | boot_b  (Android boot image, unverified because unlocked)
             payload = U-Boot (this project) -> extlinux on system_a | system_b
             (or: payload = Linux Image.gz + DTB, booted by LK directly)
```

Nothing here writes BOOT0/BOOT1, `lk_*`, `tee*`, `expdb`, `kb` or `dkb`.
Kaeru's fastboot lives in partitions this project never touches, so it is
always available for recovery: hold **Action** while plugging in power.

### What LK requires of a boot image

Taken from a stock `boot_a` and from strings in LK:

| Field | Value |
|---|---|
| Header | Android boot image v0, page size 2048 |
| Kernel | 512-byte MediaTek `KERNEL` header (magic `0x58881688`) + gzip(arm64 Image) + appended DTBs |
| DTB selection | LK picks the appended DTB whose **first** root compatible matches the board exactly (`amzn,lidar-pvt` on retail units). It copies it to `0x4e000000` (max 0x80000 bytes), fixes up memory and cmdline, and passes it in x0 |
| Entry | 64-bit, EL2, at `0x40080000`. BL31 occupies `0x43000000` + 0x30000, so the Image plus BSS must end below that |
| Cmdline | must start with `bootopt=64S3,32N2,64N2`, or LK does not take the 64-bit path |
| Ramdisk | required; plain gzip, no MediaTek header (an empty cpio works) |

`tools/lidarboot.py` builds exactly this (`pack`) and takes images apart
(`unpack`). It checks the arm64 magic, the BL31 overlap, the DTB size and the
DTB compatible. A stock image round-trips byte-identically after
decompression.

## Layout

| Path | What |
|---|---|
| `lidar-uboot/` | U-Boot v2026.10 overlay: `mt8516_amazon_lidar_defconfig`, board DTS, `MTK_LK_CHAINLOAD` Kconfig, boot environment `board/mediatek/mt8516/amazon_lidar.env`, `drivers/mmc/mtk-sd.c` (upstream copy with DMA for MT8516 and a bounce buffer for unaligned reads; check against upstream on version bumps) |
| `lidar-linux/` | Linux v7.3 overlay: board DTS, clock/reset fixes, RT5738 support in `rt5739.c`, LED fix, audio (AFE, machine driver, codec changes), `drivers/bluetooth/btmtksdio.c` (upstream copy with an MT7668 LE fix; check against upstream on version bumps), `drivers/tty/serial/8250/8250_mtk.c` (upstream copy without 16650 sleep mode, which wedges the UART's transmitter when data arrives on a closed port; check against upstream on version bumps), bindings |
| `lidar-wifi/` | MT7668 gen4 Wi-Fi driver ported from Amazon's GPL release, built out of tree |
| `lidar.config` | kernel config fragment applied on top of arm64 `defconfig` |
| `build.sh` | build entry point inside the container (targets listed in its header) |
| `Containerfile` | Debian build container with cross compilers and tools |
| `tools/lidarboot.py` | boot image pack/unpack for Amazon LK |
| `tools/mk-testimg.sh` | ext4 test system: kernel, DTB, Alpine initramfs with shells on UART and USB |
| `tools/alpine-fetch.py` | installs Alpine aarch64 packages into the test initramfs |
| `tools/devmem.c`, `tools/lidar-regdump.sh` | register access and a diffable dump of the SoC state for bring-up |
| `tools/lidar-bootanim.sh` | light-ring boot animation: twin comets over a drifting colour wheel, then a rainbow sweep and fade when boot completes |
| `tools/lidar-feature.sh` | service switches (core on, extras off by default) and settings in `/etc/lidar.conf` |
| `tools/lidar-listen.c` | TCP listener bound to one interface (`SO_BINDTODEVICE`); runs telnetd per connection |
| `tools/lidar-usbaudio.sh` | USB speakerphone: adds a UAC2 function to the gadget and bridges it to speaker and mics |
| `tools/lidar-volume.sh` | system volume daemon: USB host slider, volume buttons and CLI share one level and mute |
| `tools/lidar-ring.sh` | LED ring after boot: red while the mics are muted, volume arc on changes, the blue arc of Bluetooth pairing mode; hands the ring to `lidar-viz` in between |
| `tools/lidar-viz.c` | sound display on the LED ring (`visualizer` service): level arc or spectrum of whatever the speaker plays |
| `tools/lidar-aec.cc` | mics with the speaker's echo and background noise removed (WebRTC AEC3), for the speakerphone and later voice use |
| `tools/lidar-radio.sh` | the EFR32 radio for Zigbee and Thread at once: uploads the multi-PAN RCP if needed and runs `cpcd`, `zigbeed` with its TCP bridge, and `otbr-agent`, with both networks kept on the booted slot |
| `tools/lidar-bluetooth.sh` | Bluetooth speaker: bluetoothd, bluealsa and its player, pairing mode on the action button with LED ring and sounds, the phone's volume tied to `lidar-volume`, pairing keys kept on the booted slot |
| `tools/lidar-efr32.sh` | the EFR32's Gecko bootloader from the SoC: start the application, upload an image, exchange bytes with the application |
| `tools/lidar-wifi.sh` | Wi-Fi client from `wifi ssid`/`passphrase` (`wifi` service): start, stop, status and scan; used at boot and by the web interface |
| `tools/lidar-fireos.sh` | the files lidar takes from Fire OS (Wi-Fi firmware, speaker tuning): kept on the booted slot, staged at boot, backup export and import |
| `tools/lidar-time.sh` | the clock (no RTC on this board): NTP once the network is up, the last synced time saved on the booted slot and set at the next boot |
| `tools/lidar-xmodem.c` | XMODEM-CRC sender for the EFR32's Gecko bootloader |
| `tools/lidar-dsp.c` | speaker tuning as on Fire OS (`dsp` service): volume-dependent FIR EQ, parametric EQ, 4-band compressor/limiter, from the owner's Fire OS configuration |
| `tools/webui/` | web interface (`webui` service): `index.html` (the whole frontend) and `api.cgi` (BusyBox httpd CGI) |
| `tools/lidar-player.c` | plays the web interface's song library on the speaker, with fades on pause, stop and skip |
| `ucm2/MediaTek/amazon-lidar/` | ALSA UCM profile (alsa-ucm-conf layout) for PipeWire/PulseAudio: base routing as BootSequence, Speaker and Mic devices; the test image applies its BootSequence at boot |
| `pogo-jig/` | 3D-printable pogo-pin jig for the USB test points, held by one M4 screw in the bottom insert: OpenSCAD source, STLs, assembly notes and a test-fit checklist |
| `efr32/` | the EFR32MG12's multi-PAN RCP and `zigbeed` for the Echo (Gecko SDK 4.4.3, their own build container and a GBL packer); see `efr32/README.md` |

The overlays are copied over pristine upstream trees by `build.sh`. The
porting reference was Amazon's GPL release `Echo_Plus_2nd_Gen_src-6.5.7.1-20251021`,
which is not included.

## Building

Run from the repository root (bash on Linux, or Git Bash on Windows):

```sh
podman build -t lidar-build -f Containerfile .
podman volume create lidar-src
podman run --rm -v lidar-src:/src lidar-build bash -c '
  git clone --depth 1 -b v7.3 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git /src/linux &&
  git clone --depth 1 -b v2026.10 https://source.denx.de/u-boot/u-boot.git /src/u-boot'

W="$(pwd -W 2>/dev/null || pwd)"
run() { podman run --rm -v lidar-src:/src -v "$W:/work" lidar-build bash /work/build.sh "$@"; }
run uboot               # -> out/lidar-uboot.img
run linux lidar         # Image.gz, dtbs, modules in /src/build/lidar
# The zigbee and thread services need the EFR32's multi-PAN RCP and zigbeed,
# built in their own container (Silicon Labs' toolchain; see efr32/README.md)
# before testimg picks them up:
podman build -t efr-build -f efr32/Containerfile efr32
podman run --rm -v efr-src:/efr -v "$W:/work" efr-build bash /work/efr32/build.sh    # -> out/efr32/lidar-mp-rcp.gbl
podman run --rm -v efr-src:/efr -v "$W:/work" efr-build bash /work/efr32/zigbeed.sh  # -> out/zigbeed/zigbeed
run testimg lidar       # -> out/lidar-test-system.ext4 (also builds cpcd and ot-br-posix)
run bootimg lidar       # -> out/lidar-linux-boot.img (kernel booted straight from LK)
run patches             # -> out/patches/{linux,u-boot}-lidar.diff
```

The kernel boots without `clk_ignore_unused`: it gates every clock that is
on but unclaimed at the end of boot (the NFI, SPI-NOR, BSI, AES, SEJ, RBIST,
MSDC1 and debug gates, the APLL1 dividers, `memslp_dlyer` and `apxgpt`).
Tested on hardware with these gated: eMMC, I2C, microphones, the speaker (by
ear, with and without the speaker tuning), USB, LEDs, the light sensor,
Wi-Fi, Bluetooth and the EFR32 UART.

## Installing

1. **U-Boot.** Hold **Action** while plugging in (Kaeru fastboot), then:

   ```sh
   fastboot flash boot_a out/lidar-uboot.img
   fastboot reboot
   ```

   With an empty or unbootable system slot, U-Boot falls through to its own
   USB fastboot. Otherwise hold **Volume Up** at power-on to get there.
2. **Test system.** In U-Boot fastboot:

   ```sh
   fastboot -S 64M flash system_b out/lidar-test-system.ext4
   fastboot oem run:"setenv lidar_slot b; saveenv"
   fastboot reboot
   ```

   This erases `system_b`. If your Fire OS runs from slot b, use `system_a`
   and `lidar_slot a` instead.
3. **Shell.** The test system exports a root shell on a USB ACM gadget
   (`/dev/ttyACM0` on Linux hosts, a COM port on Windows). It also exports a
   USB NCM network interface: the device is `169.254.7.2/16`, so give the
   host's interface any other link-local address. If the `telnet` extra is
   on (the test image enables it), a second root shell without
   authentication listens on `telnet 169.254.7.2` for when the serial shell
   is busy. By default it only accepts connections arriving on `usb0`; see
   `telnet listen` under Services.

**Previous boot's log:** after a warm reboot or a kernel panic (the kernel
reboots 10 s after a panic), `/sys/fs/pstore/` holds the previous boot's
console (`console-ramoops-0`) and crash report (`dmesg-ramoops-0`). The log
lives in a reserved RAM region at `0x60000000`, below U-Boot's fastboot
download buffer (`0x60100000`) and outside its kernel and initramfs load
area.

**Recovery:** hold **Action** while plugging in, then
`fastboot flash boot_a <your boot_a dump>` restores the stock boot path.

## Services

Services sit on top of the base system, in two groups:

- **Core** services make the device work on its own, as on stock: system
  volume, LED ring, Wi-Fi, the Bluetooth speaker, the speaker tuning and the
  boot animation. They are **on by default**. A system that brings its own
  (its own volume handling or LED daemon, say) turns the matching service
  off so the two do not fight over the same controls.
- **Extras** are for development and integration: everything on the USB
  port beyond the serial shell and network (speakerphone, web interface,
  telnet) and the Zigbee and Thread radio. They are **off by default**.

Services do not require each other, with one exception: `thread` with
`thread infra wlan0` (the default) needs `wifi` and does not start without
it (it says so in the kernel log). Otherwise one that misses another falls
back and logs it: Bluetooth without `volume` lets the phone's volume scale
the samples, `dsp` without `volume` (or without Amazon's tuning files)
leaves the speaker untuned. Wi-Fi and Bluetooth share one chip: with either
on, the Wi-Fi firmware is staged (from Fire OS on `system_a`, or lidar's
copy of it: see Files from Fire OS) and the WLAN driver loads, which
Bluetooth needs first; `wifi` off then only means no network is joined.
Without the firmware there is neither, and the Bluetooth services wait
quietly.

The former `wireless` service is now `wifi`: in `lidar.conf`,
`feature.wireless` becomes `feature.wifi` and `wireless.ssid`,
`wireless.passphrase`, `wireless.country` become `wifi.ssid` and so on.
Its `wireless bluetooth` setting is gone: the `bluetooth` service, now on
by default, decides (`lidar-feature disable bluetooth` keeps the radio
off).

The switches and settings live in `/etc/lidar.conf` on the root filesystem
of the slot that booted, one `key=value` per line: `feature.<name>=0|1` and
`<name>.<key>=<value>`. This file belongs to the OS. The U-Boot environment
in `misc` only holds boot state (`lidar_slot`, `bootcount`,
`upgrade_available`). Because the file is per slot, an updater that wants
to keep the owner's choices copies it into the new slot.

Earlier builds stored these in the U-Boot environment
(`lidar_feature_*`, `lidar_setting_*`); those variables are ignored now.
Flashing a slot's image replaces its `/etc/lidar.conf`, so the image's
choices (telnet included) come back. Changing a service needs a shell on
the device, or editing the file in the image before flashing.

The test image ships an `/etc/lidar.conf` that also enables the `usbaudio`
and `telnet` extras. U-Boot passes the booted slot as
`lidar.slot=${lidar_slot}` on the kernel command line, and the test image
mounts that slot read-only at `/media/system`. If it cannot, the defaults
apply.

```sh
lidar-feature list                 # group, current state and defaults
lidar-feature disable usbaudio     # takes effect on the next boot
lidar-feature reset usbaudio       # back to the default (off)
```

| Service | Group | Default | What it does |
|---|---|---|---|
| `bootanim` | core | on | light-ring animation while booting |
| `volume` | core | on | system volume for the speaker (below) |
| `ring` | core | on | LED ring after boot: red while the mics are muted, volume changes, Bluetooth pairing mode (below) |
| `wifi` | core | on | Wi-Fi client: joins the network in the `wifi` settings at boot; the clock comes from NTP once online |
| `bluetooth` | core | on | Bluetooth speaker for phones (below) |
| `dsp` | core | on | speaker tuning as on Fire OS (below); needs the `volume` service and Amazon's tuning files (Fire OS on `system_a`, or lidar's copy), takes effect on reboot |
| `usbaudio` | extra | off | USB speakerphone (below) |
| `webui` | extra | off | web interface on port 80 of the USB network (below) |
| `telnet` | extra | off | root shell without authentication on port 23 of the USB network (`169.254.7.2`) |
| `zigbee` | extra | off | Zigbee coordinator for Home Assistant (ZHA) on TCP port 6638 (below); runs alongside `thread` on the same radio |
| `thread` | extra | off | Thread border router for Home Assistant, REST API on port 8081 (below); runs alongside `zigbee` on the same radio |
| `visualizer` | extra | off | sound display on the LED ring (below) |

Settings are refused if invalid, and a missing or invalid entry means the
default.

```sh
lidar-feature settings [feature]          # values in effect, defaults, allowed
lidar-feature get telnet listen           # prints the value in effect
lidar-feature set wifi country DE         # takes effect on the next boot
lidar-feature unset wifi country          # back to the default
```

| Setting | Default | Allowed | What it does |
|---|---|---|---|
| `telnet listen` | `usb` | `usb`, `any` | `usb`: only connections arriving on `usb0` are accepted. `any`: every interface, so the passwordless root shell is open to the Wi-Fi network too |
| `wifi country` | unset | two capital letters | regulatory domain, requested with `iw reg set` once the WLAN driver is loaded. Unset leaves the driver's and firmware's default |
| `wifi ssid` | unset | 1-32 bytes | Wi-Fi network (WPA2-Personal) to join at boot, with `wifi passphrase`; the address comes from DHCP (`udhcpc`) |
| `wifi passphrase` | unset | 8-63 printable ASCII characters | passphrase of that network. Stored in plain text in `lidar.conf`, which only root can read; `lidar-feature settings` shows it as `(set)` |
| `volume max_attenuation` | 0 | 0-60 | cap in dB: full volume is this far below the codec's 0 dB. 0 is Amazon's maximum; raise it to keep the speaker quieter |
| `volume initial` | 30 | 0-100 | volume at boot, in percent of the range up to the cap |
| `volume ring` | `on` | `on`, `off` | show volume changes on the LED ring (needs the `ring` service) |
| `volume sound` | `on` | `on`, `off` | short tone at the new level when the volume buttons change it |
| `volume keepalive` | `on` | `on`, `off` | `off`: no silent stream holding the speaker output open. No idle amplifier hiss, but every sound then clicks at its start and end |
| `ring auto_brightness` | `on` | `on`, `off` | ring brightness follows the ambient light sensor, as on stock; `off`: always full |
| `ring min_brightness` | 15 | 5-100 | ring brightness in a dark room, in percent of full |
| `usbaudio mic_gain` | 70 | 0-80 | `ADC1`-`ADC4` `PGA Capture Volume` in 0.5 dB steps: 70 is +35 dB |
| `usbaudio aec` | `on` | `on`, `off` | `on`: the microphone the host gets is cleaned by `lidar-aec` (echo cancellation, noise suppression); `off`: the plain six-mic mix |
| `zigbee listen` | `usb` | `usb`, `any` | `usb`: the coordinator port only accepts connections on `usb0`; `any`: on every interface, Wi-Fi included |
| `thread infra` | `wlan0` | `wlan0`, `usb0` | the network Home Assistant is on: the border router routes between it and the Thread network and advertises the Thread network there. `wlan0` needs the `wifi` service |
| `thread listen` | `usb` | `usb`, `any` | `usb`: the REST API sits on a unix socket that `lidar-listen` bridges to port 8081 for connections arriving on `usb0` only (`SO_BINDTODEVICE`, as for telnet); `any`: otbr-agent listens on every address, Wi-Fi included. The API has no authentication and can change the Thread network |
| `bluetooth name` | `lidar` | 1-48 bytes | name phones show for the Echo |
| `bluetooth pair_seconds` | 120 | 30-600 | how long pairing mode lasts |
| `bluetooth sound` | `on` | `on`, `off` | a double blip every 3 s in pairing mode, a chime when a phone pairs or connects, a falling tone when pairing mode ends without one |
| `visualizer style` | `level` | `level`, `spectrum` | `level`: an arc from LED 1 down both sides, green, yellow, red at the end, with a peak mark; longer with louder music and a higher volume. `spectrum`: 7 bands from 50 Hz to 10 kHz, mirrored, bass at LED 1 and treble opposite, red to violet |
| `webui listen` | `usb` | `usb`, `any` | `usb`: the web interface only accepts connections on `usb0`; `any`: on every interface, Wi-Fi included |
| `webui storage_mb` | 256 | 64-512 | RAM for uploaded songs, in MB (about 11 minutes of music per 128 MB), a RAM disk of its own; the library is cleared on reboot |

- The telnet restriction is a socket bound to `usb0` with `SO_BINDTODEVICE`
  (`tools/lidar-listen.c`), not a packet filter: the kernel only matches
  the listener for connections that come in on `usb0`, whatever address
  they are sent to. If `usb0` is missing, telnet does not start. The USB
  host, and whatever that host forwards onto the USB network, still gets a
  root shell without a password. Not yet verified on hardware.
- `usbaudio mic_gain` is applied when the speakerphone starts
  (`lidar-usbaudio start`). Old `usbaudio.speaker_volume` and
  `usbaudio.keepalive` lines in `lidar.conf` are ignored: speaker volume and
  the keep-alive belong to the `volume` service.
- `wifi country` has not been tried on hardware.
- `wifi ssid`/`passphrase`: verified on hardware. The configuration
  that `wpa_supplicant` gets (SSID as hex, the PSK from `wpa_passphrase`,
  also for passphrases with `"`, `#` and spaces) and a real join from the
  web interface: WPA2 on 5 GHz, address, default route and DNS from DHCP.
- The clock: the board has no real-time clock (no PMIC on it, as on
  stock), so it starts at 1970 on every boot. `lidar-time` sets the last
  time it saved before the services start, and BusyBox `ntpd` sets the
  real time from `pool.ntp.org` once there is a default route and a DNS
  server; every sync is saved on the booted slot. Verified on hardware:
  the saved time at 2.7 s after boot, NTP at 10 s over Wi-Fi. The clock
  runs in UTC.

### System volume

`lidar-volume` (the `volume` service) owns one speaker volume and mute, applied
to the codec's `PCM Playback Volume` in 0.5 dB steps up to the safety cap.
Volume 0 is silence: the codec's lowest step (-63.5 dB) is still audible, so
0 mutes like the mute flag (in `lidar-dsp`'s samples with the speaker tuning,
otherwise through the codec's DAC-to-driver switches). Everything that
changes volume feeds the same state:

- **USB host:** the speakerphone offers the host a hardware volume and mute
  whose steps are the codec's steps, so the host's own volume curve applies
  unchanged. Changes made on the Echo are reported back, so the host's slider
  follows them.
- **Bluetooth phone** (`bluetooth` service): the phone's volume buttons and
  slider set the level, and changes made on the Echo move the phone's
  slider (see Bluetooth speaker).
- **Volume buttons:** 3 dB per press; holding repeats after 0.4 s, one step
  every 0.15 s. A short 520 Hz tone plays at the new level on the press and,
  after a hold, once at the final level, as on stock. Host slider changes
  stay silent: the host plays its own sound.
- **Command line:** `lidar-volume get | set <percent> | up | down | mute |
  unmute | toggle`.

With the `ring` service, every change shows as a white arc on the LED ring
(the leading LED dims in between, so each step is visible; while the speaker
is muted the same arc shows dimmed, so the level it comes back at stays
visible), which goes back to the ring's normal state 2 s after the last
change.

The service also holds the speaker output open with a silent stream, as stock
keeps its output open between sounds. Measured with the Echo's own mics:
every time the codec stream starts or stops, it clicks, either because the
SoC stops the DAC's clocks or, if the amplifier is muted around that,
because the amplifier's mute pin itself clicks when it switches. With the
stream held open, sounds start at once and without clicks. Expect a faint
amplifier hiss, as on stock.

Without the `volume` service the speaker stays at the test image's base level
(-40 dB), and the USB host gets no hardware volume, so it scales the audio
itself.

### Speaker tuning

Fire OS does not play audio flat: its audio library (`libasp.so`) runs every
speaker stream through a chain configured by
`/system/vendor/etc/audio-algorithms/AFE.cfg`. The `dsp` service runs the
same chain, reverse engineered from that library, on the owner's own copy of
these files (from `system_a` or lidar's copy of them, at boot; nothing is redistributed):

1. the volume, applied digitally before everything else, as on stock;
2. a 2048-tap FIR equalizer chosen by the level (`EQ_50/90/100.cfg`): the
   speaker's response correction plus loudness compensation, +10 dB of bass
   up to -23.5 dB, +5 dB up to -4 dB, none above; a level change crosses
   zones with a 400 ms crossfade;
3. the parametric EQ (`ParametricEQ.cfg`: bass shelves +3 dB at 70 Hz and
   +9 dB at 110 Hz, -3 dB at 160 Hz);
4. the multiband compressor/limiter (`MBCL.cfg`): +6 dB in, four bands
   (100/220/5000 Hz Linkwitz-Riley crossovers), a slow RMS compressor and a
   2 ms look-ahead peak limiter per band, a full-band limiter, a hard clip;
5. both channels mixed to mono for the single speaker.

So with the service on, the same volume percentage is noticeably louder and
fuller than without, as on stock, and the compressor only works hard near
full volume. The limiters keep the output below digital full scale; the
safety cap still applies, since the codec sits at the cap and the volume is
taken off digitally. The line-out jack gets the same tuned mono signal
(stock bypasses the chain for external outputs).

How it runs: the mixed speaker stream (dmix) goes into `snd-aloop`, clocked
by the codec, and `lidar-dsp` processes it in 8 ms frames into the codec
(32 bit). `lidar-volume` writes the level to `/run/lidar-dsp/volume` and
parks the codec at the cap only while `lidar-dsp` runs and reports that its
output is live, so a stopped DSP can only make the speaker quieter. While
it is stopped, sound clients wait; init restarts it. `lidar-dsp` holds the
codec open all the time, so `volume keepalive off` makes no difference with
it. It adds about 60 ms of latency and takes about 13% of one core at
1.3 GHz. Without the `volume` service or the tuning files the speaker stays
on the plain path. Toggling the service takes effect on the next boot.

Verified: the impulse response at low level matches the stock FIR and EQ
times the MBCL band gains (+11 dB below 100 Hz, +8 dB above 220 Hz), the
limiter keeps full-scale noise below 1.0, and on the device the processed
stream arrives at the codec as computed offline. By ear, against the plain
path at the same volume: clearly fuller, closer to stock, without
distortion, clicks or dropouts.

### Files from Fire OS

Two things lidar cannot ship, because they are Amazon's and not
redistributable, come from the owner's Fire OS: the Wi-Fi firmware (which
Bluetooth needs too) and the speaker tuning. `lidar-fireos` keeps a copy of
both on the booted slot (`/var/lib/lidar-fireos`, like the pairing keys):
at every boot it refreshes the copy from `system_a`, writing only what
changed, and stages the copy. Once it exists, a wiped `system_a` no longer
matters.

- **Back it up.** An Echo whose Fire OS is gone and that has no copy has no
  Wi-Fi, no Bluetooth and no tuning until a backup is imported: `lidar-fireos
  export <file>` / `import <file>`, or "Download backup" / "Restore backup"
  under System in the web interface (USB network only). Never publish it.
- **What is needed** (tested on hardware by removing files): of the Wi-Fi
  files only the two RAM code images. Without `EEPROM_MT7668.bin` the driver
  uses the chip's own eFuse calibration; without `wifi.cfg` its defaults;
  without `TxPwrLimit_MT76x8.dat` power is limited by the kernel's
  regulatory database alone (set `wifi country`). Each of these still gave
  the same 5 GHz link at 866 Mbit/s. The tuning needs `AFE.cfg`.
- **Import checks:** only the known file names as plain files (no links,
  no paths), the RAM code images at plausible sizes, and a tuning that
  `lidar-dsp` accepts; otherwise nothing is written.
- The copy lives on one slot: a new slot starts from Fire OS again, or from
  an import.

### Web interface

With the `webui` service, `http://169.254.7.2/` on the USB host shows the
Echo's state and plays music on it, also without Wi-Fi or Bluetooth set
up:

- **Dashboard:** the three board temperature sensors, CPU load and clock,
  memory, the light sensor, the supply rails, USB traffic, the privacy
  switch, whether the speaker tuning runs, and the LED ring as it is lit.
- **Volume:** the system volume and mute (`lidar-volume`).
- **Music:** songs are dropped onto the page; the browser decodes them (any
  format it can play), converts them to 48 kHz 16-bit stereo and uploads
  them. `lidar-player` plays the library in order, with pause, seek and
  skip. The library lives in RAM (`webui storage_mb`) and is gone after a
  reboot.
- **Network:** Wi-Fi status (network, signal, band, bit rate, addresses),
  a scan list to pick a network from, connect (saved as `wifi ssid` and
  `wifi passphrase` and joined at once, through `lidar-wifi`) and forget.
  With the `bluetooth` service (it loads the Bluetooth driver): power and a
  scan of nearby devices (name, address, type, signal) through `btmgmt`,
  the speaker's state, pairing mode on and off, and the paired phones with
  a button to forget each. Thread and Zigbee state: role, network, channel, PAN ID,
  border routing.
- **Services:** switch services on and off and edit their settings, with
  inputs that only offer the allowed values; then reboot from the page.
- **Debug:** the kernel log with a filter and live follow, the previous
  boot's log from pstore (after a crash or a warm reboot), processes,
  storage, mounts and modules, and a reboot button.
- **Files:** a read-only file browser with text preview (first 256 KB) and
  download. Device nodes, `/proc/kmsg`, `/proc/kcore`, process memory and
  debugfs are refused: reading some of them blocks, and debugfs register
  dumps of clock-gated blocks can stall the bus. Everything else is
  served, `lidar.conf` with the Wi-Fi passphrase included: the settings
  view shows it as `(set)`, but the file holds it in plain text.

There is no login: as with telnet, the server (BusyBox `httpd` behind
`lidar-listen`) only accepts connections that arrive on `usb0` unless
`webui listen any` is set. Even then, everything that changes the device or
reads more than its status (Wi-Fi and Bluetooth control, services and
settings, logs, files, reboot) is only answered for clients on the USB
network (`169.254.0.0/16`); over Wi-Fi the dashboard, music and the network
status stay available. Unlike telnet, a web server can also be reached by
other web pages open in the host's browser, so the API only takes commands
as POST with `Content-Type: application/json`, or `application/octet-stream`
for those that carry data (uploads, Wi-Fi credentials, setting values),
which a foreign page cannot send without a CORS preflight that the server
never answers, and only when the Echo is addressed by IP (no DNS
rebinding). Everything on the page comes from `/cgi-bin/api` as JSON, so
scripts can use it too:

```sh
curl 'http://169.254.7.2/cgi-bin/api?stats'
curl 'http://169.254.7.2/cgi-bin/api?network'
curl -X POST -H 'Content-Type: application/json' -d '{}' 'http://169.254.7.2/cgi-bin/api?volume=40'
printf 'MyNetwork\nmy passphrase' | curl -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @- 'http://169.254.7.2/cgi-bin/api?wifi_connect'
```

### Bluetooth speaker

With the `bluetooth` service, phones pair with the Echo and play on its
speaker (A2DP sink, SBC and AAC). `bluetoothd` (BlueZ) runs the stack,
`bluealsa` the audio transport, and `bluealsa-aplay` plays every connected
phone into `lidar_speaker`: at the system volume, through the speaker
tuning, mixed with other sounds (`lidar-bluetooth`).

- **Pairing mode:** hold the action button 3 s (or "Pairing mode" in the
  web interface, or `lidar-bluetooth pair`). The Echo is then visible and
  pairable for `bluetooth pair_seconds` (120 s), a blue arc circles on the
  LED ring and a soft double blip sounds every 3 s. Playback pauses: a
  connected phone gets an AVRCP Pause, the web interface's player pauses;
  nothing resumes it afterwards. Pairing needs no PIN. The first phone that
  pairs (also again, after it forgot the Echo) ends the mode with two blue
  flashes and a rising chime. Holding the button 3 s again (or "Stop
  pairing", or `lidar-bluetooth pair stop`) ends it early; that, or nobody
  pairing in time, ends it with a falling tone. The sounds play at the
  system volume, not while it is muted, and `bluetooth sound off` turns
  them off.
- **Connections:** a paired phone is trusted and can connect at any time,
  without pairing mode; the chime greets it. Outside pairing mode the Echo
  is neither visible nor pairable, and a phone that is not trusted is
  disconnected and removed as soon as it opens an audio stream (the kernel
  lets a phone that pairs without bonding through, so trust is the gate).
  The Echo itself never connects to a phone: not at boot, and not after
  the link was lost (BlueZ's `ReconnectAttempts` is 0).
- **Volume:** the phone's volume (AVRCP absolute volume) is the system
  volume (without the `volume` service, bluealsa scales the phone's audio
  by it instead). Changes on the phone go to `lidar-volume` (with the volume arc on
  the ring), and the volume buttons, the web interface and the USB host's
  slider move the phone's slider. Each slider answers a change with its own
  rounding of it (an iPhone has 16 steps); `lidar-volume` takes what comes
  back from a slider within 1.5 s of its own write as that echo, otherwise
  the phone and the USB host push the level back and forth. A real change
  on that slider inside those 1.5 s is lost the same way; the next one
  counts again.
- Pairing keys live in `/var/lib/bluetooth`, bound from the booted slot
  (`lidar-feature keep`). Paired phones are listed and removed in the web
  interface or with `lidar-bluetooth devices` and `lidar-bluetooth forget
  <address>`.
- `bluetoothd`, `bluealsa`, `bluealsa-aplay` and the pairing agent
  (`bt-agent`, NoInputNoOutput) log to the kernel log, prefixed with their
  names. Remote-control keys a phone sends (AVRCP) reach Linux input through
  `uinput`.
- Verified on hardware with an iPhone: pairing with the action button
  (ring and sounds), ending pairing mode by a second hold and in the web
  interface, playback paused by pairing mode, the chime on connecting, AAC
  playback, volume both ways, connecting again after a reboot without
  pairing mode.

### Sound display

With the `visualizer` extra, the LED ring shows what the speaker plays
(`lidar-viz`). It listens to the AFE's loopback of the DAC input
(`hw:amazonlidar,2`), so every source counts: Bluetooth, the web player,
the USB host, system sounds. 40 frames per second.

The loopback is taken after the speaker tuning and before the codec: with
`dsp`, `lidar-dsp` has already applied the system volume (up to the cap);
on the direct path the codec applies it afterwards. `lidar-viz` adds what
is missing from `/run/lidar-volume.state`, so both paths look the same.

- `level` (default) shows the level at the speaker plus half the volume's
  gain once more, -45 dB dark to -6 dB full: louder music and a higher
  volume make the arc longer, and it gets short quickly as the volume goes
  down (dark at low volume).
- `spectrum` runs a 2048-point FFT and measures each band against its own
  reference level, which follows the loudest recent level and halves in
  4 s, so it looks the same at any volume.
- The ring service's own displays win: while `lidar-ring` shows the privacy
  red, the volume arc or pairing it holds `/run/lidar-ring.busy`, and
  `lidar-viz` keeps off the ring. Brightness follows the light sensor as the
  rest of the ring (`/run/lidar-ring.level`).
- Dark while the speaker is muted, at volume 0, and while nothing plays
  (silence is judged before the volume).
- While the USB host records through the speakerphone (`lidar-usbaudio`'s
  mic pipe runs, also with `usbaudio aec off`), `lidar-aec` may need the
  loopback: `lidar-viz` lets go of it and waits.
- Verified on hardware: both styles with music over Bluetooth, the volume
  arc, mute, privacy red and pairing taking the ring and handing it back.

### Zigbee and Thread

The EFR32 serves a Zigbee network and a Thread network at the same time, as
on Fire OS. It runs a multi-PAN radio co-processor (RCP) built from `efr32/`
(Silicon Labs' multiprotocol RCP from the Gecko SDK, adapted to this board),
and both stacks run on the Echo (`lidar-radio`):

- `cpcd` (Silicon Labs' CPC daemon, v4.4.6) shares the radio's serial line
  (`ttyS2`, 115200, no flow control) between the two stacks.
- `zigbee` service: `zigbeed` (EmberZNet 7.4.3 for Linux, built by
  `efr32/zigbeed.sh`) is the coordinator, and its EZSP port is bridged to
  TCP port 6638, the way Silicon Labs USB sticks are used. In Home Assistant,
  add the ZHA integration with the radio type EZSP and the path
  `socket://169.254.7.2:6638`. The port accepts one client at a time; when
  the client leaves, zigbeed restarts with the bridge.
- `thread` service: `otbr-agent` (ot-br-posix v2026.11.0 with Silicon Labs'
  CPC interface, built by `mk-testimg.sh`) is the border router. In Home
  Assistant, add the OpenThread Border Router integration with the URL
  `http://169.254.7.2:8081` (or the Wi-Fi address with `thread listen any`).
  `ot-ctl` is the command line on the device.

Either service runs alone or both together.

- **One channel for both.** The MG12 cannot listen on two channels at once,
  so the Zigbee and the Thread network must use the same channel: form the
  Zigbee network on the Thread network's channel (`ot-ctl channel`), or move
  the Thread network to the Zigbee channel. Their PAN IDs must differ (both
  stacks pick random ones).
- The RCP image ships in the test image
  (`/usr/share/lidar/efr32/lidar-mp-rcp.gbl`). `lidar-radio` uploads it
  through the EFR32's Gecko bootloader (`lidar-efr32`) only when `cpcd` finds
  no RCP answering: nothing there, Fire OS's Zigbee NCP or an older image of
  this project. That takes about 20 s.
- The bootloader starts the application only while `EFR32_SWCLK` (GPIO1) is
  high; low keeps it in the bootloader. The board holds the line low at
  power-on, so the EFR32 always comes up in the bootloader; `lidar-radio`
  then raises the line (`gpioset`) and starts the application with the
  bootloader's run command. After a reboot without a power cut the RCP keeps
  running.
- The networks (keys, frame counters, tables) live in `/var/lib/zigbee`
  (zigbeed's `host_token.nvm`) and `/var/lib/thread`, bound from the same
  directories on the booted slot. The slot's filesystem is writable
  underneath, but its mount at `/media/system` stays read-only, so these
  directories are the only places written; `lidar-feature` switches only its
  own mount's flag when it writes.
- An existing Zigbee network moves with ZHA's backup: back it up (or use
  "Migrate radio") before the RCP replaces the radio's firmware, then
  restore it to this coordinator. A network formed by Fire OS has no ZHA
  backup; its devices have to be paired again.
- zigbeed restarts itself when ZHA resets it, and ZHA waits only 2.5 s for
  the answer (then it retries in the wrong EZSP frame format). As built by
  the SDK, zigbeed answered after 4.1 s here; `efr32/zigbeed.sh` patches its
  restart down to 1.1 s (see `efr32/README.md`).
- `zigbeed`, the RCP, `cpcd` and its library (which `otbr-agent` links)
  are Silicon Labs' software under their license (MSLA). They are built from
  their sources on your machine and not distributed; neither is the test
  image that contains them.
- `thread infra` is the network shared with Home Assistant (`wlan0` by
  default, which needs the `wifi` service with `wifi ssid` and `wifi passphrase`). The
  service turns on IPv6 forwarding and keeps router advertisements accepted
  on that interface, as ot-br-posix's own setup does. mDNS is OpenThread's
  built-in one; the border agent appears as "lidar-mainline Thread Border
  Router".
- Multicast between the Thread network and the LAN is routed by
  otbr-agent (the kernel has `CONFIG_IPV6_MROUTE` for it). There is no
  ingress firewall: ot-br-posix's default one runs the `ipset` and
  `ip6tables` tools with rules from its own setup scripts, which this
  system does not have, so it is built without (`OT_FIREWALL=OFF`). The Echo
  then routes between the LAN and the Thread network like any IPv6 router.
- `cpcd`, `zigbeed` and `otbr-agent` log to the kernel log, one line per
  record, prefixed with their names.
- Verified on hardware with `thread infra usb0`: the RCP replaces the
  previous image, `cpcd`, `zigbeed` and `otbr-agent` run together, ZHA's
  library (`bellows`) forms a Zigbee network on channel 22 while the Thread
  network stays `leader` on channel 22, and after a reboot both networks
  come back by themselves. Not yet verified: devices joining either network,
  `wlan0` as the infrastructure link, and stability over days (Home
  Assistant dropped its own zigbeed-based add-on over stability problems).

### LED ring

`lidar-ring` (the `ring` service) owns the LED ring once the boot animation
is done. As on stock, the whole ring is red while the microphones are muted
by the privacy button (the button's own red LED is switched by the hardware
latch). Other programs show short overlays through `/run/lidar-ring.fifo`;
`lidar-volume` uses it for the volume arc. Muting or unmuting the mics ends
an overlay at once, so the privacy state always shows immediately.

As on stock, the ring dims in the dark. The service sets the TSL2540 to its
most sensitive range (highest gain, 0.72 s integration) and maps its counts
on a log scale: about 30 or less (covered, dark room at night) gives
`ring min_brightness`, 8000 (a flashlight on the top) and more give full
brightness; a dimly lit room (about 550) gives about 60 %. A steady red ring
follows the room light every 2 s, in steps of at least 10 % so it does not
flicker. The ring's own light does not reach the sensor (measured).

### USB speakerphone

The USB host sees the Echo as a USB Audio Class 2 sound card, with no driver
needed: "Speakers (Echo Plus speakerphone)" and "Microphone (Echo Plus
speakerphone)" on Windows. PC audio plays on the Echo speaker (verified by
ear), and mics 1-6 summed to mono come back as the microphone, both at
48 kHz.
`lidar-usbaudio start|stop|status` controls it at runtime.

- The audio function is added next to the serial shell and network. If the
  host had configured the gadget before but does not configure it again
  within about 15 s, the function is removed and the shell and network come
  back. This rollback path has not been exercised on hardware.
- With the `volume` service's keep-alive on (the default), the speaker pipe
  stays up while the speakerphone exists, mixed into the speaker output that
  `lidar-volume` holds open, so short sounds start at once. Without it the
  pipe only runs while the host plays: no idle hiss, but each stream clicks
  at start and end and loses up to a second of its start. The mic pipe only
  runs while the host records, which the gadget's `Playback Rate` control
  reports.
- The microphone goes through `lidar-aec` (setting `usbaudio aec`): mics 1-6
  mixed to mono, with WebRTC's AEC3 echo cancellation against the playback
  loopback (a bit-exact copy of what the DAC gets, on the same clock),
  noise suppression and a high-pass filter. The speaker plays the mono mix,
  so the reference is mixed to mono too. The echo reaches the mics only
  2.4 ms after the reference, too little for AEC3's delay estimator: without
  a margin it settled on about 100 ms and never converged. The mic is
  therefore run 40 ms behind the reference. Measured with music at 60 %
  volume: the echo drops from -31 dBFS at the mics to about -72 dBFS, the
  noise floor, after 4-5 s of adaptation (AEC3: ERL 20 dB, ERLE 15 dB).
  It costs about a third of one core while the host records.
- Two USB kernel fixes in `drivers/usb/musb/`:
  - Isochronous transfers through MUSB's Inventra DMA failed on MT8516 (IN
    produced no packets; root cause unknown), so isochronous endpoints use
    PIO there: a quirk (`MUSB_G_ISOC_NO_DMA`) set by the MediaTek glue only.
  - Isochronous packets that fail their CRC (`DATAERROR`) are dropped
    instead of being passed on with a garbage length. That had shifted the
    PC-to-Echo stream by single bytes, which played as static. Each drop
    costs 1 ms of audio. Over the pogo-pin connection to the test pads they
    happen several times per second, which a soldered, twisted D+/D- pair
    should reduce.
- Not yet done:
  - beamforming
  - clock-drift compensation: the mic and USB clocks are independent, so
    latency can creep up to about 0.7 s before the pipe resets

## U-Boot behaviour

- U-Boot runs from wherever LK put it: it is position independent and carries
  an arm64 Image header. It does not reinitialise DRAM, PLLs or the CPU rail.
- The memory map stops at `0x7f9c0000`, so the TEE carve-out is never mapped.
- The watchdog is not auto-started.
- **Volume Up held at power-on** enters U-Boot USB fastboot on the pogo-pin
  USB. If no OS slot boots, U-Boot also ends up there. The boot0/boot1
  hardware partitions and the raw `mmc0` user area are not exposed. Named GPT
  partitions are, so do not flash `lk_*`, `tee*`, `expdb`, `kb` or `dkb`.
- **No UART input is needed.** In U-Boot fastboot,
  `fastboot oem run:<command>` runs any U-Boot command, and
  `fastboot oem console` reads back its output over USB. Autoboot has no
  delay, and only the exact string `lidar` would stop it, so noise on a
  floating RX pin cannot strand U-Boot at a prompt (`bootdelay 3` in the
  environment opens a window for UART work).
- The OS comes from `/boot/extlinux/extlinux.conf` on ext4 `system_${lidar_slot}`.
- A/B fallback uses `bootcount`, `bootlimit=3` and `upgrade_available`
  (verified on hardware: after three unconfirmed boots U-Boot switched
  `lidar_slot` and still came up through the other slot). They
  are stored in the environment at the top of `misc`: two 64 KiB copies at
  partition offsets `0x70000` and `0x60000`. Amazon only uses the first 6 KiB
  of `misc`. For `fw_setenv` in the OS, `/etc/fw_env.config` is:

  ```
  /dev/disk/by-partlabel/misc 0x70000 0x10000
  /dev/disk/by-partlabel/misc 0x60000 0x10000
  ```

  To update, write the inactive slot, then run
  `fw_setenv lidar_slot b; fw_setenv upgrade_available 1; fw_setenv bootcount 0`
  and reboot. Confirm the boot from userspace with
  `fw_setenv upgrade_available 0; fw_setenv bootcount 0`.

Useful commands from the PC:

```sh
fastboot oem run:"mmc info";         fastboot oem console
fastboot oem run:"part list mmc 0";  fastboot oem console
fastboot oem run:"button list";      fastboot oem console
```

## Bring-up checklist

From the test system's shell:

- `dmesg`
- `/sys/kernel/debug/gpio`
- `i2cdetect -y 0/1/2`: expect 0x18-0x1b, 0x39, 0x3f, 0x48 on bus 0; 0x57 on
  bus 1; and 0x18, 0x48, 0x4a on bus 2. Addresses with a bound driver show
  as `UU`, which does not prove the chip is there: 0x2e (MCP4631) shows `UU`
  too, but every read from it fails with no ACK
- `cat /sys/class/hwmon/*/temp1_input`
- LEDs in `/sys/class/leds/ring*:*/brightness`
- `evtest` for the buttons and the privacy switch
- the SDIO host MSDC2 (`&mmc2`, which appears as `mmc1`) enumerating the
  MT7668 as `mmc1:0001:1` in `/sys/bus/sdio/devices`

### Speaker tests

Start quiet: `amixer cset name='PCM Playback Volume' 47,47` (-40 dB), or
`lidar-volume set 20` with the `volume` service. The test image sets the
output drivers as Amazon does (line-out 0 dB, headphone +5 dB), so PCM
volume 127 (0 dB) is the speaker's full stock loudness. The control goes
up to 175 (+24 dB), and a desktop sound server using the UCM profile is not
bound by `lidar-volume`'s cap.

## Status

- **Boot time:** U-Boot records its stages against the SoC counter, which
  runs from reset, and hands them to Linux (`/proc/device-tree/bootstage`).
  Measured with a PC on the USB port: U-Boot starts 12.1 s after reset,
  Linux at 14.5 s, the boot finishes about 4 s later. The static blue ring
  is LK's, as on stock, and stays until the kernel takes over the LEDs.
  - Preloader (9.2 s in a stock UART log): 8 s of it wait for a flash tool
    because a USB host is attached (`USB enum timeout (8000ms)`). With only
    the power supply there is no such wait. A host with a driver bound to
    the preloader port (VID `0e8d`, PID `2000`; Linux does this by itself)
    cuts it to the 2.5 s handshake timeout.
  - LK with Kaeru, about 3 s: a 1.7 s key check and 0.8 s reading boot_a.
    These stages are not changed here (`lk_*` is never written).
  - U-Boot, 2.4 s: init 1 s, loading kernel and initramfs 0.7 s, unpacking
    0.7 s. Three changes took Linux from 17.5 s to 14.5 s after reset:
    - no autoboot delay;
    - the initramfs only carries the modules in
      `tools/initramfs-modules.txt` and their dependencies (111 of 801),
      which also cut the kernel's unpacking from 1.4 s to 0.95 s;
    - U-Boot's eMMC driver uses DMA on MT8516 (it ran PIO): about 40 MB/s
      instead of 13 MB/s. Verified before switching it on: DMA reads of
      kernel and initramfs were byte-identical to PIO reads, and a DMA
      write to a free area of `misc` read back identically.

    Installs from before this keep the autoboot delay saved in their
    environment; remove it with `fw_setenv bootdelay 0`.
- **Vproc / cpufreq:** working. The RT5738 reads die ID 0 and vendor ID 0
  (RT5739-class path). VSEL (GPIO9) is an input pulled high on the board, so
  the active bank is NSEL1 (`richtek,vsel-active-high`), as on stock.
  `mediatek-cpufreq` scales 598 MHz to 1.3 GHz. NSEL1 follows the clock in
  hardware: `0xb4` (1.20 V) at 1.04 GHz and `0xc8` (1.30 V) at 1.3 GHz,
  stable under a 4-core load.
- **CPU idle:** the stock kernel never uses PSCI cpuidle while more than one
  core is online; it only executes `wfi`. Its deep idle needs kernel-loaded
  SPM firmware plus CIRQ, and runs from the last online CPU only. With
  mainline's idle states the SoC never wakes. The board DT drops
  `cpu-idle-states`, which leaves plain WFI as on stock.
- **Wi-Fi/BT (`mmc2`):** three upstream bugs are fixed, and the controller now
  probes:
  1. The MSDC2 `source` and `hclk` clocks were swapped in `mt8516.dtsi`.
  2. `clk-mtk` sized the clock table by count, so IDs above 174
     (`MSDC2_INFRA`) were unreachable and the SDIO engine gate stayed off. The
     reset then spun forever.
  3. The infracfg reset bank (INFRA_RST0, bit 23 = MSDC2) was missing.

  The host setup matches Amazon's `mtk-sdio.c` in power sequence, clocks,
  divider, pins, pulls and drive. The test system stages the firmware and
  loads `wlan_mt76x8_sdio`, then `btmtksdio`, without manual steps. Verified
  on hardware: Wi-Fi scans on 2.4 and 5 GHz and connects (tested on 5 GHz:
  866 Mbit/s link, about 150 Mbit/s HTTPS download), Bluetooth finds classic
  and LE devices, both at the same time.
- **Wi-Fi and Bluetooth firmware:** both functions share one ROM patch.
  Amazon's (`mt7668_patch_e2_hdr.bin`, a 2025 build) crashes the Bluetooth
  firmware under mainline `btmtksdio`: its first LE or vendor command ends in
  a prefetch abort and a core dump streamed as packet type 0x80. The test
  image therefore ships the redistributable `mediatek/mt7668pr2h.bin` from
  linux-firmware (pinned by SHA-256) and uses it for Wi-Fi too. The Wi-Fi RAM
  code, calibration and settings stay owner-local: they come from the
  device's own Fire OS partition, and lidar keeps a copy on its slot (Files
  from Fire OS). Never commit them. See `lidar-wifi/README.md`.
- **Bluetooth LE:** linux-firmware's patch leaves octets 25-28 of Read Local
  Supported Commands empty, i.e. all Bluetooth 4.0 LE commands, although it
  executes them. The core then never enables the LE Advertising Report and
  Connection Complete events, and LE scans find nothing. The `btmtksdio`
  overlay fills these octets in for the MT7668 when they arrive empty.
- **Wi-Fi / Bluetooth load order:** load `wlan_mt76x8_sdio` before
  `btmtksdio` (`softdep btmtksdio pre: wlan_mt76x8_sdio`). Mainline btmtksdio
  reads "patch download in progress" as "done", so bringing up `hci0` during
  the WLAN firmware download can fail.
- **Wi-Fi MAC:** stock reads it from Amazon IDME, which LK places as an
  `/idme` node in the devicetree it hands over. U-Boot copies
  `/idme/mac_addr` from there into `local-mac-address` on `&mmc2/wifi@1`
  (verified on hardware). The driver falls back to `wifi.cfg`, then eFuse,
  then a random `00:08:22:xx:xx:xx`.
- **LED ring:** LED n uses channels 3n-2, 3n-1 and 3n (R, G, B), named
  `ringN:red/green/blue`. Mainline `leds-is31fl32xx` was missing
  `brightness_steps` for the IS31FL3236, so brightness writes were dropped;
  this is fixed. The controller is the IS31FL3236A variant, run at 22 kHz
  PWM (`issi,22khz-pwm`) as stock does: the default 3 kHz was audible as a
  whine through the speaker whenever LEDs were lit.
- **Microphones:** all 7 mics capture with the `DIF_2L_3L`/`DIF_2R_3R`
  inputs and PGA 20 dB. Room noise is coherent between mic 7 (capture
  channel 6) and mics 1-6. Mic 7 also picks up narrow interference tones the
  other mics do not, so it needs filtering. Their frequencies differ between
  units: about 50 Hz, 130-144 Hz and 2.41 kHz on one, about 65, 120, 152 and
  316 Hz on another. Capture channel 7 is the line-in, which has not been
  tested with a real source.
- **Speaker:** a two-way system. The DAC's left channel drives the woofer
  and its right channel the tweeter, through the crossover Amazon's firmware
  loads into the DAC biquads:
  - woofer: low-pass, -3.5 dB at 1 kHz, inverted polarity
  - tweeter: fourth order high-pass, -3.4 dB at 3 kHz

  Both channels play the mono mix of the stream, from the line-out drivers.
  The machine driver loads the filters whenever the output route is Speaker
  (`DAC Biquad Coefficients`, `DAC Mono Mix Switch`), refuses the speaker
  route if they cannot be set, and limits playback to 48 kHz, the rate the
  coefficients are designed for. The DAC's MFP5 pin mutes the amplifier,
  which has no status output of its own. Measured with a USB microphone
  over the Echo: enabled unmuted, the amplifier pops loudly about 0.4 s
  after the enable; enabled muted it is silent, and an unmute is heard
  about 0.4 s after the pin changes. So it is enabled muted and unmuted
  500 ms later. Each direction still makes one faint click (about -40 dBFS
  at the microphone) that neither the order nor a longer delay (up to 2 s)
  removes: the unmute, and on power-down whichever of disabling and muting
  comes first (the other one is then silent). The driver disables first
  and then mutes, so the amplifier is muted for the next start. With the
  speaker output held open (`volume keepalive`) that is one click at boot
  and one at shutdown or reboot. Fire OS uses the same order: its audio HAL
  (`libalexaaudiohal.so`) enables the amplifier, unmutes it 400 ms later,
  and on close disables it and then mutes it. The HAL's speaker tables set
  nothing else on the amplifier, and the stock device tree gives the
  speaker path only GPIO105 (amplifier enable) and GPIO109 (output mux),
  with no amplifier supply regulator. The MCP4631 potentiometer at 0x2e on
  I2C bus 2, also in the stock device tree, does not acknowledge any read
  on the tested unit, so it plays no part in the amplifier sequence.
- **Line-out / headphones:** both channels work through the `Output Route`
  control (GPIO109 low = jack, amp off), now from the headphone drivers in
  plain stereo as on stock. Before that change every stream start made a
  loud crack; it has not been re-tested since.
- **Mic privacy:** the hardware latch is exposed as `SW_MUTE_DEVICE` (GPIO18)
  and the button as `KEY_MICMUTE` (GPIO108). Software can force privacy on
  through GPIO107 (active low). This is left released, and the line is free
  for userspace.
- **EFR32 (Zigbee + Thread):** needs no kernel driver. It talks to the SoC
  on `ttyS2` (115200, no flow control), with a Silicon Labs Gecko Bootloader
  (v1.0.0, XMODEM upload) in front of the application. Fire OS's
  `zb_migration_manager` shows the control lines: `EFR32_SWCLK` (GPIO1,
  rfkill `zigbee_swclk`) is the bootloader trigger (low = stay in the
  bootloader) and `EFR32_RESET_N` (GPIO13, rfkill `zigbee_reset`) the reset.
  Fire OS carries two images: `silabs/xncp-fw.gbl` (EmberZNet 6.5.3 NCP over
  ASH/EZSP) and `dsr/zb_mux_rcp.gbl` (DSR's OpenThread "dualpan" RCP), both
  unencrypted. Verified on hardware: with `EFR32_SWCLK` low, a pulse on
  `EFR32_RESET_N` brings up the bootloader menu, and its `2. run` starts the
  application again. ttyS2 needs the `8250_mtk.c` overlay: without it the
  UART stops transmitting for good once the EFR32 sends while the port is
  closed.
- **TSL2540 ambient light sensor:** new minimal IIO driver (`tsl2540`):
  visible and IR counts, gain and integration time. Verified on hardware:
  chip ID, configuration, and readings from 0 in the dark to about 7000
  counts under a torch (128x gain, 360 ms). Values are relative: the
  datasheet has no lux equation and the cover calibration is not ported.
  The `ring` service sets it to 256x gain and 0.72 s integration for its
  brightness control; with `ring auto_brightness off` the driver default
  (1x, 0.18 s) stays.

## Upstreaming

The overlays still need these changes before they go upstream as patch
series:

- Split the work into series: bindings, `clk-mtk` table sizing, `clk-mt8516`
  reset bank and `GATE_TOP5`, `mt8516.dtsi` MSDC2 clocks, `leds-is31fl32xx`,
  RT5738, `tlv320adc3xxx`, AFE, machine driver, board DTS.
- Document the `amzn` vendor prefix and the board compatible. LK requires
  `amzn,lidar-pvt`.
- Rework the board DT:
  - multicolor LED nodes
  - no pinctrl-based line levels (`board_hog_pins`)
  - a `dt-bindings` define for the MSDC2 reset index
- U-Boot: a `MAINTAINERS` entry and a board doc. The defconfig comments go
  there.

## Licensing

| Path | Licence |
|---|---|
| `lidar-linux/` | per file, as in the Linux kernel (mostly `GPL-2.0-only`; new bindings `GPL-2.0-only OR BSD-2-Clause`) |
| `lidar-uboot/` | per file, as in U-Boot (`GPL-2.0+`; DTS `GPL-2.0 OR MIT`) |
| `lidar-wifi/` | MediaTek dual licence, `GPL-2.0 OR BSD-3-Clause` |
| `ucm2/` | `BSD-3-Clause`, as alsa-ucm-conf |
| `pogo-jig/` | `CERN-OHL-S-2.0` |
| everything else | `GPL-2.0-only` (see `LICENSE`) |

Licence texts are in `LICENSES/`.
