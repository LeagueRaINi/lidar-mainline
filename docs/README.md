# lidar-mainline documentation

[Project README](../README.md) · [Licensing](../LICENSING.md)

| Page | What it covers |
|---|---|
| [Install](install.md) | prerequisites, flashing U-Boot and the test system with `fastboot`, the shell over USB, the previous boot's log, recovery to the stock boot path, the browser installer |
| [Building](building.md) | the build container, `build.sh` targets, the EFR32 firmware build, releases, pinned downloads and `tools/alpine.lock` |
| [Status](status.md) | every feature compared with stock Fire OS, and what upstreaming still needs |
| [Hardware](hardware.md) | test points (and the 17 V pad), hardware status by block (boot time, CPU, Wi-Fi/Bluetooth, LED ring, microphones, speaker, EFR32, light sensor, USB), LED ring orientation, bring-up checklist, speaker tests |
| [U-Boot and the boot chain](uboot.md) | the boot chain, what Amazon LK requires of a boot image, U-Boot's behaviour, fastboot `oem` commands, A/B updates with `fw_setenv` |
| [Services](services.md) | `lidar-feature`: core and extra services, `/etc/lidar.conf`, every setting, files from Fire OS, the LED ring service, Zigbee and Thread |
| [Audio](audio.md) | system volume, speaker tuning (Fire OS's DSP chain), the Bluetooth speaker, the sound display on the LED ring |
| [Web interface](webui.md) | the views, its JSON API, language, first-run setup, login, network access |
| [Voice](voice.md) | wake word, voice assistant, Claude, speech models, speech recognition, speech output |
| [Development](development.md) | repository layout: what each directory and tool is |

Elsewhere in the repository: [`efr32/README.md`](../efr32/README.md) (the
Zigbee/Thread radio firmware and `zigbeed`), [`pogo-jig/README.md`](../pogo-jig/README.md)
(the pogo-pin jig) and [`lidar-wifi/README.md`](../lidar-wifi/README.md)
(the out-of-tree Wi-Fi driver).

The banner of the project README is rendered from
[`banner/banner.html`](banner/banner.html) (instructions inside).
