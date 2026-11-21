# EFR32MG12 radio firmware

A multiprotocol RCP for lidar's Silicon Labs radio: Zigbee and Thread at the
same time on one channel (OpenThread multi-PAN RCP over CPC), driven by
`cpcd`, `zigbeed` and `otbr-agent` on the Echo (see the main README).

## The radio, as read from Amazon's two images

Fire OS ships `silabs/xncp-fw.gbl` (EmberZNet 6.5.3 NCP) and
`dsr/zb_mux_rcp.gbl` (an OpenThread "dualpan" RCP, release 1.22, behind DSR's
own multiplexer). Both are plain, unsigned GBL v3 files.

| Item | Value | Evidence |
|---|---|---|
| Chip | EFR32MG12, 1 MB flash, 128 KB RAM | application at 0 (Series 1 with a separate bootloader area); stack top 0x20020000; every MG12 with 128 KB has 1 MB |
| Host link | USART0, TX PA0, RX PA1, no CTS/RTS | `GPIO_PinModeSet` calls and `USART0->ROUTEPEN = 3` in the RCP image |
| PA5 | driven high at start-up | `GPIO_PinModeSet` in the RCP image; done here through the SDK's VCOM-enable board control |
| Crystal | 38.4 MHz | constant in both images |
| Supply | DC-DC to DVDD | `EMU->PWRCFG = 2` and the SDK's DC-DC init constants |
| Radio | 2.4 GHz high-power PA from 3.3 V, 10 µs ramp | RAIL TX power config in both images |
| Crystal trim | factory token in the USERDATA page, used by the SDK when present | |

The layout matches Silicon Labs' Thunderboard Sense 2 (BRD4166A) apart from
the chip variant (that board has 256 KB of RAM), the PA supply and flow control.

## Build

```sh
podman build -t efr-build -f efr32/Containerfile efr32
podman run --rm -v efr-src:/efr -v "$PWD:/work" efr-build bash /work/efr32/build.sh
```

Output: `out/efr32/lidar-mp-rcp.gbl` and `.elf`. Nothing of Silicon Labs' SDK
is in this repository; `build.sh` fetches it pinned:

- Gecko SDK v4.4.3: the last SDK with Series 1 (MG12) support. Mostly under
  Silicon Labs' MSLA, the radio library under zlib.
- slc-cli 6.0.22, its Java runtime and Python, from Silicon Labs' update site.

`extension/` is an SLC extension: the board (`lidar_board`, chip variant and
config files) and the project `lidar-mp-rcp.slcp`. The project is the SDK's
`rcp-uart-802154` sample (its `main.c` and `app.c` are used in place) with
the lidar board, the bootloader application properties, no flow control, no
CPC security and no packet trace. slc writes a makefile project, built with
the SDK's own rules. Three config headers (HFXO, PA, CPC UART) are Silicon
Labs' BRD4166A ones, zlib-licensed, with marked changes;
`sl_board_control_config.h` is our own.
`mkgbl.py` packs the image as a GBL the way Amazon's files are laid out; it
reproduces Amazon's RCP image byte for byte in its application and program
tags.

The image, as built:

| Item | Value |
|---|---|
| Host link | CPC over USART0, 115200 8N1, TX PA0 / RX PA1 (`ROUTELOC0 = 0`, `ROUTEPEN = 3`), no CTS/RTS, no CPC security |
| CPC protocol | version 5, as cpc-daemon v4.4.6 |
| Spinel | multi-PAN (`OPENTHREAD_CONFIG_MULTIPAN_RCP_ENABLE=1`), version `SL-OPENTHREAD/2.4.3.0_GitHub-7074a43e4` |
| Flash | 120.5 KB of code from 0; NVM3 in the last 36 KB of main flash (0xF7000) |
| RAM | 27 KB static (stack 2752, heap 2048 minimum) of 128 KB |

slc 6 notes, all handled in `build.sh`:
- it only loads extensions that are real directories inside the SDK's
  `extension/` folder;
- it needs the extension's declared SDK version to match exactly;
- it finds its Python through `STUDIO_PYTHON3_PATH`.

## Status

- Verified on hardware: `lidar-radio` uploads the image through the
  bootloader, `cpcd` connects, and `zigbeed` (IID 1) and `otbr-agent` (IID 2)
  run on it together: a Zigbee network formed by ZHA's library and a Thread
  network, both on channel 22, both back after a reboot.
- The bootloader uses the same pins (checked in its code read from
  0x0FE10000): `GPIO_PinModeSet` PA0 push-pull, PA1 input, PA5 push-pull high,
  `USART0->ROUTELOC0 = 0`, `ROUTEPEN = 3`.
- The host side needs the `8250_mtk.c` overlay in
  `lidar-linux/drivers/tty/serial/8250/`: without it ttyS2 stops transmitting
  once the EFR32 sends while the port is closed, and only a reboot of the Echo
  clears that state.

## Going back to Amazon's firmware

Fire OS's images can go back by hand through the bootloader. Disable the
`zigbee` and `thread` services and reboot first: otherwise cpcd holds
`ttyS2`, and `lidar-radio` uploads its own image again at the next start.
Then hold `EFR32_SWCLK` low, pulse `EFR32_RESET_N`, choose `1` in the
bootloader menu and send the image with `lidar-xmodem` (or
`lidar-efr32 flash <gbl>`). The bootloader and the chip's factory data are
never written.

## zigbeed

The Zigbee stack for the multiprotocol RCP runs on the Echo: `zigbeed`
(EmberZNet 7.4.3 from the Gecko SDK) reaches the radio through `cpcd` and
offers the usual EZSP/ASH serial protocol (EZSP version 13) on a PTY.

```sh
podman run --rm -v efr-src:/efr -v "$PWD:/work" efr-build bash /work/efr32/zigbeed.sh
```

Output: `out/zigbeed/zigbeed`, a static aarch64 program (2 MB). The script
generates the SDK's own project (`protocol/zigbee/app/zigbeed/zigbeed.slcp`
with `zigbee_arm64`, unchanged stack configuration) and links it against
libcpc from cpc-daemon v4.4.6. Two sources are compiled from patched
copies, to make zigbeed answer a reset from the host in time (ZHA's
`bellows` waits 2.5 s, then retries in the wrong EZSP frame format; the SDK's
zigbeed took 4.1 s):

- `app.c`: a reset makes zigbeed `execvp` itself again, and the SDK sleeps a
  second before the new program connects to cpcd; the sleep is gone
  (libcpc's sockets are close-on-exec, nothing is left over to wait for).
- `cpc_interface.cpp`: the interface answers the radio's Spinel RESET itself,
  but OpenThread still blocked in one read for 2 s waiting for a reply that
  never comes; right after that answer the read no longer blocks.

Measured over TCP 6638 on hardware, from the ASH RST to zigbeed's RSTACK:
4.1 s before, 1.1 s with them.

The stack exists only as Silicon Labs' prebuilt arm64 libraries, compiled
against glibc, so everything is linked statically with Debian's glibc cross
toolchain. The libraries use nothing but ISO C functions (`malloc`,
`memcpy`, `printf`, `time`, …) and the program never resolves users or host
names, so static glibc has nothing to load at run time and works on the musl
userland.

Running it:

```sh
cd /var/lib/zigbee && exec zigbeed \
    -r 'spinel+cpc://cpcd_0?iid=1&iid-list=0' -p /run/zigbeed.pty -d 2 -v
```

- `-r`: radio URL; the host part is the `cpcd` instance, whose sockets
  libcpc expects under `/dev/shm/cpcd/cpcd_0/` (fixed at build time).
  `cpcd` must be running, otherwise zigbeed exits with status 1.
- `-p`: the EZSP serial port, default `/tmp/ttyZigbeed`. zigbeed opens it,
  it does not create it: `socat` has to provide the PTY first, for example
  `socat pty,raw,echo=0,link=/tmp/ttyZigbeed tcp-listen:6638,reuseaddr`.
  When that `socat` ends (client gone), restart zigbeed with it.
- Network data (tokens) are kept in `host_token.nvm` in the current
  directory, a file mapped shared and synced on every write; hence the `cd`
  to persistent storage.
- Logging goes to syslog (`daemon` facility); `-v` copies it to stderr.
  `-d 0`…`5` sets the level of the Spinel layer; it takes effect once the
  radio is up (without it, this build logs every Spinel frame).
- The same options can be given as `name=value` lines in
  `/usr/local/etc/zigbeed.conf` (`radio-url=…`, `ezsp-interface=…`); the
  "Cannot open file" message for a missing one is harmless.
- An EZSP reset makes zigbeed `execvp` its own `argv[0]` again. Start it by
  name through `PATH`, as `lidar-radio` does: `pgrep -x zigbeed` (status, web
  interface) only matches that name.
