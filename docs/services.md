# Services

[Documentation](README.md) · [Project README](../README.md)

Services sit on top of the base system, in two groups:

- **Core** services make the device work on its own, as on stock: system
  volume, LED ring, Wi-Fi, the Bluetooth speaker, the speaker tuning and the
  boot animation. They are **on by default**. A system that brings its own
  (its own volume handling or LED daemon, say) turns the matching service
  off so the two do not fight over the same controls.
- **Extras** are for development and integration: everything on the USB
  port beyond the serial shell and network (web interface, telnet) and the
  Zigbee and Thread radio. They are **off by default**.

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

An image you build yourself ships an `/etc/lidar.conf` that also enables
the `telnet` extra; a release's is empty. U-Boot passes the booted slot as
`lidar.slot=${lidar_slot}` on the kernel command line, and the test image
mounts that slot read-only at
`/media/system`. If it cannot, the defaults apply.

```sh
lidar-feature list                 # group, current state and defaults
lidar-feature disable telnet       # takes effect on the next boot
lidar-feature reset telnet         # back to the default (off)
```

| Service | Group | Default | What it does |
|---|---|---|---|
| `bootanim` | core | on | light-ring animation while booting |
| `volume` | core | on | system volume for the speaker ([System volume](audio.md#system-volume)) |
| `ring` | core | on | LED ring after boot: red while the mics are muted, volume changes, Bluetooth pairing mode ([LED ring](#led-ring)) |
| `wifi` | core | on | Wi-Fi client: joins the network in the `wifi` settings at boot; the clock comes from NTP once online |
| `bluetooth` | core | on | Bluetooth speaker for phones ([Bluetooth speaker](audio.md#bluetooth-speaker)) |
| `dsp` | core | on | speaker tuning as on Fire OS ([Speaker tuning](audio.md#speaker-tuning)); needs the `volume` service and Amazon's tuning files (Fire OS on `system_a`, or lidar's copy), takes effect on reboot |
| `webui` | core | on | web interface on port 80 of the USB network, with the first-run setup ([Web interface](webui.md)) |
| `telnet` | extra | off | root shell without authentication on port 23 of the USB network (`169.254.7.2`) |
| `zigbee` | extra | off | Zigbee coordinator for Home Assistant (ZHA) on TCP port 6638 ([Zigbee and Thread](#zigbee-and-thread)); runs alongside `thread` on the same radio |
| `thread` | extra | off | Thread border router for Home Assistant, REST API on port 8081 ([Zigbee and Thread](#zigbee-and-thread)); runs alongside `zigbee` on the same radio |
| `visualizer` | extra | off | sound display on the LED ring ([Sound display](audio.md#sound-display)) |
| `assistant` | extra | off | voice assistant core: timers, alarms, volume, music, time and date by typed command ([Assistant](voice.md#assistant)) |
| `voice` | extra | off | wake word "Hey Jarvis" with the beamformer, direction on the ring, a chime; about one CPU core ([Wake word](voice.md#wake-word)) |

## Settings

Settings are refused if invalid, and a missing or invalid entry means the
default. Most take effect on the next boot; the live ones apply at once,
because what reads them does so where it uses them or is told: `dsp bass`,
`mid`, `treble`, `boost`, `visualizer style` (the display restarts with
it), `ring min_brightness`, `volume sound` and
`ring`, `bluetooth sound` and `pair_seconds`, and those the web interface
applies itself (Wi-Fi network, API key, login). Init keeps a copy of
`lidar.conf` as it was at boot (`/run/lidar.conf.boot`), so `lidar-feature
pending` lists only changes that still differ from what runs: setting a
value and back again leaves nothing to reboot for.

```sh
lidar-feature settings [feature]          # values in effect, defaults, allowed
lidar-feature get telnet listen           # prints the value in effect
lidar-feature set wifi country DE         # takes effect on the next boot
lidar-feature set voice api_key < keyfile # no value: the first line of standard input
lidar-feature unset wifi country          # back to the default
lidar-feature pending                     # what still waits for a reboot
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
| `dsp bass` / `dsp mid` / `dsp treble` | 0 | -6..6 | tone in dB on top of the speaker tuning: shelf below 120 Hz, peak at 800 Hz, shelf above 6 kHz. Applies at once |
| `dsp boost` | 0 | 0-6 | loudness boost in dB into the tuning's compressor and limiter: louder at the same volume, peaks stay limited. Applies at once |
| `ring auto_brightness` | `on` | `on`, `off` | ring brightness follows the ambient light sensor, as on stock; `off`: always full |
| `ring min_brightness` | 15 | 5-100 | ring brightness in a dark room, in percent of full |
| `zigbee listen` | `usb` | `usb`, `any` | `usb`: the coordinator port only accepts connections on `usb0`; `any`: on every interface, Wi-Fi included |
| `thread infra` | `wlan0` | `wlan0`, `usb0` | the network Home Assistant is on: the border router routes between it and the Thread network and advertises the Thread network there. `wlan0` needs the `wifi` service |
| `thread listen` | `usb` | `usb`, `any` | `usb`: the REST API sits on a unix socket that `lidar-listen` bridges to port 8081 for connections arriving on `usb0` only (`SO_BINDTODEVICE`, as for telnet); `any`: otbr-agent listens on every address, Wi-Fi included. The API has no authentication and can change the Thread network |
| `bluetooth name` | `lidar` | 1-48 bytes | name phones show for the Echo |
| `bluetooth pair_seconds` | 120 | 30-600 | how long pairing mode lasts |
| `bluetooth sound` | `on` | `on`, `off` | a double blip every 3 s in pairing mode, a chime when a phone pairs or connects, a falling tone when pairing mode ends without one |
| `visualizer style` | `level` | `level`, `spectrum` | `level`: an arc from LED 1 down both sides, green, yellow, red at the end, with a peak mark; longer with louder music and a higher volume. `spectrum`: 7 bands from 50 Hz to 10 kHz, mirrored, bass at LED 1 and treble opposite, red to violet |
| `webui listen` | `usb` | `usb`, `any` | `usb`: the web interface only accepts connections on `usb0`; `any`: on every interface, Wi-Fi included |
| `webui storage_mb` | 256 | 64-512 | RAM for uploaded songs, in MB (about 11 minutes of music per 128 MB), a RAM disk of its own; the library is cleared on reboot |
| `webui setup_done` | `no` | `no`, `yes` | `yes`: the first-run setup of the web interface was finished. `no`: the web interface opens it for clients on the USB network |
| `webui auth` | `off` | `off`, `wifi`, `all` | login for the web interface ([Login](webui.md#login)). `wifi`: asked of clients that are not on the USB network. `all`: of USB clients too. Set from the web interface, which stores the two settings below with it |
| `webui auth_kind` | unset | `pin`, `password` | what the login is, so the page asks for the right thing |
| `webui auth_hash` | unset | sha512-crypt hash | hash of the PIN or password, as `cryptpw -m sha512` prints it; `lidar-feature settings` shows it as `(set)`. Without it there is no login, whatever `webui auth` says |
| `webui fireos_image_saved` | `no` | `no`, `yes` | `yes`: the owner confirmed in the setup that the Fire OS image (`system_a`) was downloaded and saved. Nothing else depends on it yet |
| `voice wake_word` | `hey_jarvis` | `hey_jarvis` | word the voice assistant listens for; the only one for now |
| `voice language` | `en` | `en`, `de` | language the voice assistant understands and speaks: English, German |
| `voice timezone` | `UTC0` | POSIX TZ string | time zone of alarms and the time, e.g. `CET-1CEST,M3.5.0,M10.5.0/3` (Central Europe) |
| `voice ring_volume` | 30 | 0-100 | volume an alarm or timer rings at, at least; the speaker is unmuted for it. 0: volume and mute stay as they are |
| `voice voice_de` | `thorsten` | `thorsten`, `eva_k` | German voice: natural and slower, or smaller and about twice as fast; installed by `lidar-models` |
| `voice sensitivity` | `medium` | `low`, `medium`, `high` | how readily the wake word triggers: `low` misses more, `high` triggers more often by mistake |
| `voice api_key` | unset | 16-200 printable ASCII characters, no spaces | Claude API key of the voice assistant. Stored in plain text in `lidar.conf`, which only root can read; `lidar-feature settings` shows it as `(set)` |

- `voice` is a group of settings without a service switch: it does not
  appear in `lidar-feature list`. The `assistant` service reads `voice
  language`, `timezone` and `ring_volume`; `lidar-wakeword` does not take
  its cutoff from `voice sensitivity` yet.
- `lidar-feature set <name> <key>` without a value reads it from standard
  input, which keeps a passphrase or key out of the process list. The web
  interface hands on setting values and the Wi-Fi passphrase that way.
  Verified on hardware with `voice api_key` (set and unset).
- The telnet restriction is a socket bound to `usb0` with `SO_BINDTODEVICE`
  (`tools/lidar-listen.c`), not a packet filter: the kernel only matches
  the listener for connections that come in on `usb0`, whatever address
  they are sent to. If `usb0` is missing, telnet does not start. The USB
  host, and whatever that host forwards onto the USB network, still gets a
  root shell without a password. Not yet verified on hardware.
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

## Files from Fire OS

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

## LED ring

`lidar-ring` (the `ring` service) owns the LED ring once the boot animation
is done. As on stock, the whole ring is red while the microphones are muted
by the privacy button (the button's own red LED is switched by the hardware
latch). Other programs show short overlays through `/run/lidar-ring.fifo`;
`lidar-volume` uses it for the volume arc. Muting or unmuting the mics ends
an overlay at once, so the privacy state always shows immediately.

`wake <led>` (from `lidar-wake`) lights the LED towards whoever said the
wake word and, dimmer, its two neighbours in cyan for 3 s or until
`wake off`. Not while the mics are muted; a volume change replaces it, and
the pairing arc continues after it.

As on stock, the ring dims in the dark. The service sets the TSL2540 to its
most sensitive range (highest gain, 0.72 s integration) and maps its counts
on a log scale: about 30 or less (covered, dark room at night) gives
`ring min_brightness`, 8000 (a flashlight on the top) and more give full
brightness; a dimly lit room (about 550) gives about 60 %. A steady red ring
follows the room light every 2 s, in steps of at least 10 % so it does not
flicker. The ring's own light does not reach the sensor (measured). A
change of `ring min_brightness` applies within seconds, without a restart.

Which LED sits where: [LED ring orientation](hardware.md#led-ring-orientation).

## Zigbee and Thread

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
  restart down to 1.1 s (see [`efr32/README.md`](../efr32/README.md)).
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
