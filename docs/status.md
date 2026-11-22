# Status

[Documentation](README.md) · [Project README](../README.md)

What works on lidar-mainline, compared with stock Fire OS, and what is
left before the kernel and U-Boot changes go upstream.

✅ working · ⚠️ works, known issue · 🧪 implemented, needs testing on
hardware · 🛠️ planned · ➖ not planned. Details are under [Hardware status](hardware.md#hardware-status).

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
| Microphones (7) | ✅ | ✅ all 7 capture, shared (`lidar_mic8`, dsnoop, 48 kHz): 6 on a ring of 32 mm radius plus one in the centre. Mic 7 (centre) once showed narrow interference tones; a quiet-room measurement now puts it within 4 dB of the ring mics, 50 Hz-8 kHz. Beamforming with echo cancellation in `lidar-voice` (experimental) |
| Echo-cancellation reference | ✅ | ✅ playback loopback capture, bit-exact, shared (`lidar_ref`, dsnoop); used by `lidar-voice` and the sound display (`lidar-viz`) |
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
| Voice assistant | ✅ Alexa | 🧪 local, experimental: wake word (`lidar-wakeword` behind `lidar-voice`) with the talker direction on the ring, speech recognition (`lidar-stt`), the assistant core (`lidar-assistant`: timers, alarms, volume, music, time and date, English and German), spoken answers (`lidar-tts`, Piper voices) and Claude for the rest (`lidar-claude`, your API key); models downloaded on the Echo (`lidar-models`) |
| Services, with settings | n/a | ✅ `lidar-feature`: core services on, extras off by default |
| Alexa cloud services | ✅ | ➖ |
| Kernel and U-Boot upstreaming | n/a | 🛠️ patch series drafted |

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
