# lidar-mainline: U-Boot and mainline Linux for the Echo Plus 2nd gen

Mainline Linux v7.3 and U-Boot v2026.10 for the Amazon Echo Plus 2nd gen
(codename "lidar", MediaTek MT8516). It covers the bootloader, the kernel, and
the driver work that runs **after** the unlock. There is no exploit or unlock
code here: the device must already run Kaeru LK, as installed by
[`davidwatters/echo-plus-2-unlock`](https://github.com/davidwatters/echo-plus-2-unlock).

Status at a glance:

| Area | Status |
|---|---|
| U-Boot as second stage (fastboot, A/B slots, environment) | working |
| eMMC, I2C, buttons, mic-mute and privacy latch, temperatures | working |
| LED ring (12 RGB LEDs) | working |
| Audio: speaker, line-out, microphones 1-6 | working (see Status) |
| CPU frequency scaling (598 MHz-1.3 GHz) | working |
| Wi-Fi / Bluetooth (MT7668 on SDIO) | host side working; untested on hardware |
| EFR32 Zigbee/Thread radio | UARTs and GPIOs exposed |

## Prerequisites

- An Echo Plus 2nd gen that is already unlocked and runs Kaeru LK.
- Your own full eMMC dump from the unlock, at least `boot_a`, for recovery.
- `fastboot` (Android platform-tools) and `podman` on the host.
- Network access while building: the test image downloads Alpine packages.
- Access to the USB test points (TM4, TM5 and TM37 below), for example with
  pogo pins. All flashing, the shell and the network go over this USB link.
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
| TM35, TM45 | unknown | near 0 V; not UART0 RX (does not follow pin 62 when it is toggled) |

UART0 RX is not on any known test point.

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
| `lidar-uboot/` | U-Boot v2026.10 overlay: `mt8516_amazon_lidar_defconfig`, board DTS, `MTK_LK_CHAINLOAD` Kconfig, boot environment `board/mediatek/mt8516/amazon_lidar.env` |
| `lidar-linux/` | Linux v7.3 overlay: board DTS, clock/reset fixes, RT5738 support in `rt5739.c`, LED fix, audio (AFE, machine driver, codec changes), bindings |
| `lidar-wifi/` | MT7668 gen4 Wi-Fi driver ported from Amazon's GPL release, built out of tree |
| `lidar.config` | kernel config fragment applied on top of arm64 `defconfig` |
| `build.sh` | build entry point inside the container (targets listed in its header) |
| `Containerfile` | Debian build container with cross compilers and tools |
| `tools/lidarboot.py` | boot image pack/unpack for Amazon LK |
| `tools/mk-testimg.sh` | ext4 test system: kernel, DTB, Alpine initramfs with shells on UART and USB |
| `tools/alpine-fetch.py` | installs Alpine aarch64 packages into the test initramfs |
| `tools/devmem.c`, `tools/lidar-regdump.sh` | register access and a diffable dump of the SoC state for bring-up |
| `tools/lidar-bootanim.sh` | light-ring boot animation: twin comets over a drifting colour wheel, then a rainbow sweep and fade when boot completes |

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
run testimg lidar       # -> out/lidar-test-system.ext4
run bootimg lidar       # -> out/lidar-linux-boot.img (kernel booted straight from LK)
run patches             # -> out/patches/{linux,u-boot}-lidar.diff
```

The kernel command line includes `clk_ignore_unused`. This is a bring-up
workaround while not every clock consumer is described in the DT.

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
   host's interface any other link-local address.

**Recovery:** hold **Action** while plugging in, then
`fastboot flash boot_a <your boot_a dump>` restores the stock boot path.

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
  `fastboot oem console` reads back its output over USB. Autoboot stops only
  on the exact string `lidar`, so noise on a floating RX pin cannot strand
  U-Boot at a prompt.
- The OS comes from `/boot/extlinux/extlinux.conf` on ext4 `system_${lidar_slot}`.
- A/B fallback uses `bootcount`, `bootlimit=3` and `upgrade_available`. They
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
  bus 1; and 0x18, 0x2e, 0x48, 0x4a on bus 2
- `cat /sys/class/hwmon/*/temp1_input`
- LEDs in `/sys/class/leds/ring*:*/brightness`
- `evtest` for the buttons and the privacy switch
- the SDIO host MSDC2 (`&mmc2`, which appears as `mmc1`) enumerating the
  MT7668 as `mmc1:0001:1` in `/sys/bus/sdio/devices`

### Speaker tests

Start quiet: `amixer cset name='PCM Playback Volume' 47,47` (-40 dB),
`'LO Driver Gain Volume' 0,0` and `'HP Driver Gain Volume' 0,0`. PCM volume
127 (0 dB) with driver gain is very loud on this speaker.

## Status

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

  On the development unit the MT7668 does not answer CMD52/CMD5. This is a
  hardware failure of that unit, unrelated to the software, so Wi-Fi and
  Bluetooth are untested on hardware. The host setup matches Amazon's
  `mtk-sdio.c` in power sequence, clocks, divider, pins, pulls and drive. The
  test system stages the firmware and loads `wlan_mt76x8_sdio`, then
  `btmtksdio`, without manual steps.
- **Wi-Fi firmware and calibration** are owner-local: the test system copies
  them from the device's own Fire OS partition at boot. Never commit them. See
  `lidar-wifi/README.md`.
- **Wi-Fi / Bluetooth load order:** load `wlan_mt76x8_sdio` before
  `btmtksdio` (`softdep btmtksdio pre: wlan_mt76x8_sdio`). Mainline btmtksdio
  reads "patch download in progress" as "done", so bringing up `hci0` during
  the WLAN firmware download can fail.
- **Wi-Fi MAC:** stock reads it from Amazon IDME. The driver takes
  `local-mac-address` on `&mmc2/wifi@1`, then `wifi.cfg`, then eFuse. Without
  any of these it falls back to a random `00:08:22:xx:xx:xx` on every boot.
  U-Boot does not fill the node from IDME yet.
- **LED ring:** LED n uses channels 3n-2, 3n-1 and 3n (R, G, B), named
  `ringN:red/green/blue`. Mainline `leds-is31fl32xx` was missing
  `brightness_steps` for the IS31FL3236, so brightness writes were dropped;
  this is fixed.
- **Microphones:** mics 1-6 respond (DIF_2L_3L/DIF_2R_3R inputs, PGA 20 dB).
  Mic 7 (capture channel 6) is noisy and does not respond yet. Capture
  channel 7 is the line-in, which has not been tested with a real source.
- **Line-out / headphones:** both channels work through the `Output Route`
  control (GPIO109 low = jack, amp off). Every stream start makes a loud
  crack. Waiting for the output-driver gain ramp, a soft start (`HEADSTART`)
  and `pmdown_time = 0` did not cure it. The next suspects are the HP vs LO
  driver choice and the codec power sequence.
- **Mic privacy:** the hardware latch is exposed as `SW_MUTE_DEVICE` (GPIO18)
  and the button as `KEY_MICMUTE` (GPIO108). Software can force privacy on
  through GPIO107 (active low). This is left released, and the line is free
  for userspace.
- **EFR32 (Zigbee + Thread):** needs no kernel driver. Zigbee EZSP is on
  `ttyS1`, and Spinel is on `ttyS2` with RTS/CTS. `EFR32_RESET_N` (GPIO13) and
  `EFR32_SWCLK` (GPIO1) are named lines, driven to their run state by pinctrl
  and left unclaimed for libgpiod.
- **TSL2540 ambient light sensor:** no mainline driver. A vendor driver
  (`drivers/misc/ams_tsl2540*.c`) is in Amazon's GPL release.

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
| everything else | `GPL-2.0-only` (see `LICENSE`) |

Licence texts are in `LICENSES/`.
