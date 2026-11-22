# Licensing

| Path | Licence |
|---|---|
| `lidar-linux/` | per file, as in the Linux kernel (mostly `GPL-2.0-only`; new bindings `GPL-2.0-only OR BSD-2-Clause`) |
| `lidar-uboot/` | per file, as in U-Boot (`GPL-2.0+`; DTS `GPL-2.0 OR MIT`) |
| `lidar-wifi/` | MediaTek dual licence, `GPL-2.0 OR BSD-3-Clause` |
| `ucm2/` | `BSD-3-Clause`, as alsa-ucm-conf |
| `pogo-jig/` | `CERN-OHL-S-2.0` |
| `tools/lidar-wakeword.cc` | `GPL-2.0-or-later`: the binary links Apache-2.0 code (below), which combines with GPL version 3 but not with version 2 alone, so the binary as a whole is under GPL-3.0-or-later terms |
| `tools/lidar-stt/lidar-stt.cc` | `GPL-2.0-or-later`, for the same reason as `lidar-wakeword.cc` (links sherpa-onnx, `Apache-2.0`) |
| `tools/lidar-tts/lidar-tts.cc`, `tools/lidar-tts/nfd.h` | `GPL-2.0-or-later`: the binary links sherpa-onnx (`Apache-2.0`) and espeak-ng (`GPL-3.0-or-later`), so as a whole it is under GPL-3.0-or-later terms |
| `tools/lidar-claude/lidar-claude.cc` | `GPL-2.0-or-later`: the binary links OpenSSL 3 (`Apache-2.0`), so as a whole it is under GPL-3.0-or-later terms |
| `installer/vendor/` | `MIT` (fastboot.js v1.1.3, unmodified, licence text beside it); the bundle contains zip.js, `BSD-3-Clause`, with its notices kept inside the file |
| everything else | `GPL-2.0-only` (see [`LICENSE`](LICENSE)) |

Licence texts are in [`LICENSES/`](LICENSES/).

Not in this repository, downloaded when the test image is built and linked
into `lidar-wakeword` or shipped beside it:

| What | Licence |
|---|---|
| TensorFlow Lite Micro, TensorFlow's microfrontend, FlatBuffers | `Apache-2.0` |
| KISS FFT | `BSD-3-Clause` |
| the `hey_jarvis` microWakeWord model and its manifest | `Apache-2.0` |

Their licence texts and the model collection's NOTICE are installed in
`/usr/share/licenses/lidar-wakeword`.

Downloaded at build time and built into `libsherpa-onnx-c-api.so` (used by
`lidar-stt`): sherpa-onnx v2.0.2 with kaldi-native-fbank, kaldi-decoder,
kaldifst, kaldi-hmm-gmm, kaldi-native-io, simple-sentencepiece and OpenFst
(`Apache-2.0`), Eigen (`MPL-2.0`, headers), nlohmann/json and utf8proc
(`MIT`), KISS FFT (`BSD-3-Clause`); licence texts in
`/usr/share/licenses/sherpa-onnx`. onnxruntime (`MIT`) is an Alpine package.
The recognizer models are downloaded on the Echo, not shipped (see
[Speech models](docs/voice.md#speech-models)).

Downloaded at build time and linked into `lidar-tts`: espeak-ng (commit
`724808c5`, the one Piper 1.8 builds with; `GPL-3.0-or-later`, its
ucd-tools too, with Unicode data under the Unicode licence), and its
compiled data for English and German in `/usr/share/espeak-ng-data`;
licence texts in `/usr/share/licenses/espeak-ng`.
`libsherpa-onnx-c-api.so` is shared with `lidar-stt`.

Releases also carry `lidar-voicecache-<voice>.tar`: the assistant's fixed
phrases rendered on the build host with the Piper voices `lidar-models`
offers. The audio follows the licences of those voices as their model cards
give them: `en_US-kristin-medium` (dataset from LibriVox, public domain),
`de_DE-thorsten-medium` (Thorsten-Voice dataset, CC0) and
`de_DE-eva_k-x_low` (trained on the M-AILABS Speech Dataset; licence as on
<https://www.caito.de/2019/01/03/the-m-ailabs-speech-dataset/>). The voices
themselves are not shipped: the Echo downloads them from
rhasspy/piper-voices at a pinned revision.

Also downloaded at build time and shipped in the image: the MT7668 ROM patch
`mediatek/mt7668pr2h.bin` from linux-firmware, under `LICENCE.mediatek`
(redistributable for devices with MediaTek chipsets; text in
`/usr/share/licenses/linux-firmware`), and the Alpine Linux packages named
in `tools/alpine.lock`, each under its own licence (`fdk-aac`, which
Bluetooth AAC uses, under Fraunhofer's FDK licence, which grants no patent
licence).
