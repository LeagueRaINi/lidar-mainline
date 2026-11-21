# EFR32MG12 radio firmware (work in progress)

Goal: Thread (OpenThread RCP, for an OpenThread border router on the Echo)
on lidar's Silicon Labs radio, later Zigbee and Thread at the same time
(multi-PAN RCP with `cpcd`, `zigbeed` and `otbr-agent`). Today the radio runs
Amazon's EmberZNet NCP for the `zigbee` service.

## The radio, as read from Amazon's two images

Fire OS ships `silabs/xncp-fw.gbl` (EmberZNet 6.5.3 NCP) and
`dsr/zb_mux_rcp.gbl` (an OpenThread "dualpan" RCP, release 1.22, behind DSR's
own multiplexer). Both are plain, unsigned GBL v3 files.

| Item | Value | Evidence |
|---|---|---|
| Chip | EFR32MG12, 1 MB flash, 128 KB RAM | application at 0 (Series 1 with a separate bootloader area); stack top 0x20020000; every MG12 with 128 KB has 1 MB |
| Host link | USART0, TX PA0, RX PA1, no CTS/RTS | `GPIO_PinModeSet` calls and `USART0->ROUTEPEN = 3` in the RCP image |
| PA5 | driven high at start-up | same function; VCOM enable on Silicon Labs' BRD4161A/BRD4166A |
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

Output: `out/efr32/lidar-ot-rcp.gbl` and `.elf`. Nothing of Silicon Labs' SDK
is in this repository; `build.sh` fetches it pinned:

- Gecko SDK v4.4.3: the last SDK with Series 1 (MG12) support. Mostly under
  Silicon Labs' MSLA, the radio library under zlib.
- ot-efr32 at `089440d`, the last revision on the Gecko SDK.
- slc-cli 6.0.22, its Java runtime and Python, from Silicon Labs' update site.

`extension/` is an SLC extension: the board (`lidar_board`, chip variant and
config files) and the RCP project (ot-efr32's UART RCP plus the bootloader
application properties, without packet trace). The config headers are
Silicon Labs' BRD4166A ones, zlib-licensed, with marked changes.
`mkgbl.py` packs the image as a GBL the way Amazon's files are laid out; it
reproduces Amazon's RCP image byte for byte in its application and program
tags.

slc 6 notes, all handled in `build.sh`:
- it only loads extensions that are real directories inside the SDK's
  `extension/` folder, so ot-efr32 lives there;
- it needs the extension's declared SDK version to match exactly;
- it finds its Python through `STUDIO_PYTHON3_PATH`.

## Status

- The image builds, the Gecko bootloader accepts it ("Serial upload
  complete"), and it starts: after a reset it sends its log lines and the
  reset notification on PA0 at 115200.
- It answers Spinel requests: `NCP_VERSION` returns
  `OPENTHREAD/089440d; EFR32; …`, `PROTOCOL_VERSION` 4.3.
- The bootloader uses the same pins (checked in its code read from
  0x0FE10000): `GPIO_PinModeSet` PA0 push-pull, PA1 input, PA5 push-pull high,
  `USART0->ROUTELOC0 = 0`, `ROUTEPEN = 3`.
- The host side needs the `8250_mtk.c` overlay in
  `lidar-linux/drivers/tty/serial/8250/`: without it ttyS2 stops transmitting
  once the EFR32 sends while the port is closed, and only a reboot of the Echo
  clears that state.
- Next: `otbr-agent` on the Echo against this RCP.

## Going back to Amazon's Zigbee firmware

Hold `EFR32_SWCLK` low, pulse `EFR32_RESET_N`, choose `1` in the bootloader
menu and send Fire OS's `xncp-fw.gbl` with `lidar-xmodem`. The `zigbee`
service does this by itself when it finds no Zigbee firmware. The bootloader
and the chip's factory data are never written.
