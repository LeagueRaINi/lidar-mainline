# Development

[Documentation](README.md) · [Project README](../README.md)

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
| `tools/alpine-fetch.py` | installs Alpine aarch64 packages into the test initramfs, pinned by `tools/alpine.lock`; `--update` rewrites the lock |
| `tools/mk-release.sh` | checks the built images (version, nothing that must not be passed on) and collects the release files with `SHA256SUMS` |
| `tools/devmem.c`, `tools/lidar-regdump.sh` | register access and a diffable dump of the SoC state for bring-up |
| `tools/lidar-bootanim.sh` | light-ring boot animation: twin comets over a drifting colour wheel, then a rainbow sweep and fade when boot completes |
| `tools/lidar-feature.sh` | service switches (core on, extras off by default) and settings in `/etc/lidar.conf` |
| `tools/lidar-listen.c` | TCP listener bound to one interface (`SO_BINDTODEVICE`); runs telnetd per connection |
| `tools/lidar-volume.sh` | system volume daemon: Bluetooth phones, volume buttons and CLI share one level and mute |
| `tools/lidar-ring.sh` | LED ring after boot: red while the mics are muted, volume arc on changes, the blue arc of Bluetooth pairing mode; hands the ring to `lidar-viz` in between |
| `tools/lidar-viz.c` | sound display on the LED ring (`visualizer` service): level arc or spectrum of whatever the speaker plays |
| `tools/lidar-voice.cc` | experimental: the voice of whoever speaks to the Echo, from all 7 mics: echo cancellation on every mic (WebRTC AEC3), 12 superdirective beams, the best beam by speech over noise, noise suppression; 16 kHz mono out, beam state for the ring and the web interface |
| `tools/lidar-wakeword.cc` | wake word detector ("Hey Jarvis") for the output of `lidar-voice`: microWakeWord model on TensorFlow Lite Micro, one line per detection ([Wake word](voice.md#wake-word)) |
| `tools/mk-wakeword.sh` | fetches the pinned TensorFlow Lite Micro, microfrontend, FlatBuffers and KISS FFT sources and the wake word model, and builds `lidar-wakeword` for the test image or for the build host |
| `tools/lidar-assistant/` | voice assistant core (`assistant` service): English and German grammar for timers, alarms, volume, music, time and date, the actions behind it (also as a tool list for a language model), spoken answers as text; `lang/*.txt` hold each language's phrases and word lists, `build.sh test` runs its tests on the build host |
| `tools/lidar-claude/` | asks Claude (Anthropic Messages API, streamed, over OpenSSL) what the assistant's grammar does not understand, with the assistant's actions as tools; speaks each sentence as it arrives |
| `tools/lidar-stt/` | speech recognition after the wake word (`lidar-stt`, Kroko on sherpa-onnx, kept loaded) and `lidar-tee`, which hands it a copy of `lidar-voice`'s audio without ever waiting; `build.sh` fetches the pinned sherpa-onnx |
| `tools/lidar-radio.sh` | the EFR32 radio for Zigbee and Thread at once: uploads the multi-PAN RCP if needed and runs `cpcd`, `zigbeed` with its TCP bridge, and `otbr-agent`, with both networks kept on the booted slot |
| `tools/lidar-bluetooth.sh` | Bluetooth speaker: bluetoothd, bluealsa and its player, pairing mode on the action button with LED ring and sounds, the phone's volume tied to `lidar-volume`, pairing keys kept on the booted slot |
| `tools/lidar-efr32.sh` | the EFR32's Gecko bootloader from the SoC: start the application, upload an image, exchange bytes with the application |
| `tools/lidar-wifi.sh` | Wi-Fi client from `wifi ssid`/`passphrase` (`wifi` service): start, stop, status and scan; used at boot and by the web interface |
| `tools/lidar-fireos.sh` | the files lidar takes from Fire OS (Wi-Fi firmware, speaker tuning): kept on the booted slot, staged at boot, backup export and import |
| `tools/lidar-time.sh` | the clock (no RTC on this board): NTP once the network is up, the last synced time saved on the booted slot and set at the next boot |
| `tools/lidar-models.sh` | speech models of the voice assistant: downloads the recognizer and the voice of one language on the device, pinned by sha256; grows the system filesystem to its partition |
| `tools/lidar-xmodem.c` | XMODEM-CRC sender for the EFR32's Gecko bootloader |
| `tools/lidar-dsp.c` | speaker tuning as on Fire OS (`dsp` service): volume-dependent FIR EQ, parametric EQ, 4-band compressor/limiter, from the owner's Fire OS configuration |
| `tools/webui/` | web interface (`webui` service): `index.html` (the whole frontend) and `api.cgi` (BusyBox httpd CGI) |
| `tools/lidar-player.c` | plays the web interface's song library on the speaker, with fades on pause, stop and skip |
| `ucm2/MediaTek/amazon-lidar/` | ALSA UCM profile (alsa-ucm-conf layout) for PipeWire/PulseAudio: base routing as BootSequence, Speaker and Mic devices; the test image applies its BootSequence at boot |
| `pogo-jig/` | 3D-printable pogo-pin jig for the USB test points, held by one M4 screw in the bottom insert: OpenSCAD source, STLs, assembly notes and a test-fit checklist |
| `efr32/` | the EFR32MG12's multi-PAN RCP and `zigbeed` for the Echo (Gecko SDK 4.4.3, their own build container and a GBL packer); see [`efr32/README.md`](../efr32/README.md) |
| `installer/` | the project's GitHub Pages site: `index.html` (landing page, `landing.css`, screenshots in `shots/`) and `install.html`, the browser installer (WebUSB, `installer.js`, `style.css`) that flashes U-Boot and the test system (see [Installing](install.md#installing)); `site.css` holds what both pages share; `installer/vendor/` holds fastboot.js; `.github/workflows/pages.yml` publishes the directory to GitHub Pages |

The overlays are copied over pristine upstream trees by `build.sh`. The
porting reference was Amazon's GPL release `Echo_Plus_2nd_Gen_src-6.5.7.1-20251021`,
which is not included.
