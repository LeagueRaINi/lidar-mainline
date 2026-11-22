# Voice

[Documentation](README.md) · [Project README](../README.md)

The voice path, all on the Echo: `lidar-voice` (the beamformer; see the
`tools/lidar-voice.cc` row in [Development](development.md#layout)) ->
[wake word](#wake-word) ->
[speech recognition](#speech-recognition) -> [assistant](#assistant)
(and [Claude](#claude)) -> [speech output](#speech-output); the models
come from [`lidar-models`](#speech-models). Settings: the `voice` group in
[Services](services.md).

## Wake word

`lidar-wakeword` listens for "Hey Jarvis" in 16 kHz 16-bit mono audio on
standard input, which is what `lidar-voice` writes:

```sh
lidar-voice | lidar-wakeword
```

It prints one line per detection, `wake hey_jarvis <probability> <seconds
since boot>`, and then stays quiet for a second. `-v` prints the
probability of every inference (one per 30 ms) on stderr, `-t 0.9` sets
another cutoff than the model's 0.97, and `-m`/`-c` select another
microWakeWord v2 model and its manifest.

The `voice` service (an extra, off by default; about one CPU core, nearly
all of it `lidar-voice`) runs this pipeline as `lidar-wake daemon` and
restarts it if it ends. At each detection it

- shows the direction of the voice on the LED ring (see [LED ring](services.md#led-ring)). It
  comes from `lidar-voice`'s `doa`, not from the beam it listens with: the
  beam selection holds a beam until another is clearly better, which for
  a short word often left it one beam off. `doa` is SRP-PHAT (phase-only
  mic spectra steered to every 10°, summed over the speech band of the
  frames with speech, fading over 1 s); a recording of "Hey Jarvis" from
  the four buttons put it within 5° of the true direction 11 times of 11,
  and confirmed the mic positions (ring mic n at 60·n° clockwise from mic
  0, mic 2 by Action) to within 2°,
- plays a short, quiet two-note chime at the system volume (none while the
  speaker is muted; the sound display keeps off it),
- appends `wake <seconds since boot> <angle>` to `/run/lidar-wake.events`
  (the last 20) and writes `/run/lidar-wake.json` (`last`, `angle`,
  `count`),
- keeps `/run/lidar-wake.listening` for 5 s: the time to say something,
  which `lidar-stt` listens in (see Speech recognition).

`voice sensitivity` sets the cutoff (low 0.99, medium the model's 0.97,
high 0.9; not yet tuned on the device). While the microphones are muted
they deliver silence: the service keeps running and hears nothing.
`lidar-wake status` prints whether it runs and the last detection.

The detector is microWakeWord's `hey_jarvis` v2 model, run as ESPHome's
`micro_wake_word` component runs it (same audio features, same decision
over the last 5 probabilities; none of its code), on TensorFlow Lite
Micro.

- **On hardware:** 5 of 5 "Hey Jarvis" detected behind `lidar-voice`, no
  false accept during the test. One short test, one speaker.
- **On a PC**, with synthetic speech (Piper, 109 voices, 154 utterances of
  "Hey Jarvis" alone and with a question run on):

  | Condition | Detected |
  |---|---|
  | no noise | 148 of 154 |
  | pink noise, 10 dB SNR | 152 of 154 |
  | pink noise, 0 dB SNR | 152 of 154 |
  | music, 5 dB SNR | 152 of 154 |

  The model is trained on Piper speech, so these numbers are optimistic;
  no recordings of people were tested on the PC. Most misses are the
  phrase run together with the question without a pause.
- **False accepts on a PC:** none in 12.2 hours (10.5 h of read speech
  from LibriSpeech, 0.5 h of music, 1.2 h of noise and silence).
- **Similar phrases** usually trigger it too ("Hey Jervis" 31 of 35 voices,
  "Hey Travis" 23 of 35); "Jarvis" alone, "Hey Siri" and "Alexa" never
  did.
- **Load:** 0.09 % of a PC core and 4.5 MB of memory. On the Echo's
  Cortex-A35 that is an estimated 1.3-1.9 % of one core, not measured.
- The arm64 binary gives the same probabilities as the PC build, byte for
  byte, over 5.5 minutes of speech and music (under `qemu-aarch64`).

Neither the model nor the TensorFlow sources are in this repository:
`tools/mk-wakeword.sh` fetches them at build time, pinned by commit or
checksum (see [Licensing](../LICENSING.md)). The licence texts go into the image, in
`/usr/share/licenses/lidar-wakeword`. `tools/mk-wakeword.sh <dir>` without
a sysroot builds the detector for the build host, to try it on recordings.

## Assistant

`lidar-assistant` (the `assistant` service, off by default) takes what was
said as text, one utterance per line on `/run/lidar-assistant.fifo`, and
carries it out: volume and speaker mute (`lidar-volume`), named timers,
alarms (once, on a weekday, weekdays, weekends or every day), time and
date, pause, resume, next and previous (`lidar-bluetooth media`, also the
web player), and "stop" for whatever rings or plays. English or German
(`voice language`); the phrases it answers with and the words it listens
for are in `/usr/share/lidar/assistant/<language>.txt`. What is said after
the wake word comes from `lidar-stt` (see Speech recognition); answers are
spoken by `lidar-tts` (see Speech output), and what the grammar does not
understand goes on as `unmatched <text>` (handed to Claude with `-c`, see
Claude).

- **Try it:** `echo "set a pizza timer for twelve minutes" >
  /run/lidar-assistant.fifo` acts at once; the answer is spoken.
  `lidar-assistant -t -d /tmp/x` shows answers as you type (on a clock of
  its own, for tests, and away from the service's alarms).
- **Timers** run on the time since boot and are gone after a reboot.
  **Alarms** are kept on the booted slot (`/var/lib/lidar-assistant`), in
  the time zone of `voice timezone` (a POSIX TZ string, e.g.
  `CET-1CEST,M3.5.0,M10.5.0/3`); they are neither set nor do they ring
  before the clock came from the network (`/run/lidar-time.synced`), and
  one more than 10 minutes late is dropped.
- **Ringing:** a two-tone chime that starts quiet and rises over 30 s, or
  `/var/lib/lidar-assistant/alarm.wav` if there is one (48 kHz 16-bit
  stereo), with the speaker unmuted and at `voice ring_volume` at least,
  until "stop", any button, or `echo '!stop' > /run/lidar-assistant.fifo`;
  10 minutes at most. The volume goes back afterwards unless someone
  changed it meanwhile. The microphones are never unmuted by voice: their
  mute is a hardware latch.
- **State:** `/run/lidar-assistant.json` (timers with `ends` in seconds
  since boot, alarms, what rings), written when it changes. It also holds
  the last utterance and what became of it (`last`: `seq`, `uptime`,
  `text`, `action`, `ok`, `say`; `null` before the first), which the web
  interface's assistant box shows.
- `lidar-assistant -S` prints the actions as a tool list for a language
  model, with their parameters and ranges; `-P` every phrase and number
  word the answers are made of, for a speech cache.
- Verified on the Echo: time, date, named timers and their remaining time,
  alarm set, list and cancel in German with the local time zone, volume;
  a timer rang and stopped at a button press. Not yet: an alarm actually
  ringing at its time, media control against a phone.

## Claude

What the assistant's grammar does not understand goes to Claude (Claude
Haiku 5.5 through the Anthropic Messages API): `lidar-assistant -c
lidar-claude` runs `lidar-claude <language> <utterance>` in the background,
and the next utterance ends it. It needs `voice api_key` (read through a
pipe; never an argument or a log line) and the network.

- Answers are one or two short spoken sentences in `voice language`, with
  the local time and date given; streamed, each sentence goes to the say
  hook (`lidar-say`, see Speech output) as soon as it is complete. With a
  key it first says that it asks ("Moment, ich frage nach."); without one,
  that it did not understand, and once per boot where the key goes.
- Claude can use the assistant's actions (`lidar-assistant -S`) as tools:
  each call goes to `lidar-assistant` as `!tool <reply FIFO> <action>
  <name>=<value>...` on its FIFO, is checked like any command and carried
  out without being spoken; the result goes back to Claude. At most four
  rounds of tools.
- 5 s to connect, 20 s for the whole answer; no key, no network, an
  invalid key, rate limits and overload are said briefly. No memory of
  earlier questions.
- Status lines (no text, no key) in the kernel log: `dmesg | grep lidar-claude`.
- Tested on a PC against a mock of the API (streaming, tools, errors, TLS);
  not yet against the real API or on the Echo.

## Speech models

The recognizer and the voice of the assistant are too large for the image
(about 130 MB per language) and are downloaded on the Echo, one language at
a time:

```sh
lidar-models install en            # or: de, de -v eva_k
lidar-models status | verify | remove en
```

- Recognizer: sherpa-onnx streaming zipformer "Kroko" (2025-08-06) from the
  sherpa-onnx releases; licence: see https://huggingface.co/Banafo/Kroko-ASR.
- Voices: Piper `en_US-kristin-medium` (LibriVox, public domain),
  `de_DE-thorsten-medium` (Thorsten-Voice, CC0, the rdh-based version) or
  `de_DE-eva_k-x_low` (M-AILABS; smaller and faster; `voice voice_de`),
  from rhasspy/piper-voices on Hugging Face at a pinned revision.
- Every file is pinned by URL and sha256 in the script. A download is
  checked before it is unpacked, the result is checked again, and only then
  the `current` symlink switches; the other language is removed afterwards.
- They live in `/var/lib/lidar-models` on the booted slot
  (`current/stt`, `current/tts`, `current/info`). Flashing a new system
  image removes them.
- The image's filesystem is 256 MB on a partition of about 1.2 GB. An
  install first grows the mounted filesystem to its partition
  (`resize2fs`; `lidar-models grow-fs` does only that): one language alone
  left 15 MB free on 256 MB, too little for the next system update. It
  only grows and does nothing if there is nothing to add.
- Progress of an install: `/run/lidar-models.progress`, one tab-separated
  line `<state> <percent> <language> <voice> <message>` with state
  `download`, `unpack`, `verify`, `done` or `error`.
- With an image from a release, the voice's phrase cache of that release
  comes too (`lidar-voicecache-<voice>.tar`, checked against its
  `SHA256SUMS`); if it cannot be fetched, the install goes on and
  `lidar-tts` prepares it.
- Tested on the PC (BusyBox ash, real downloads, grow-fs on a loop
  device) and on the Echo: install of `en` and `de`, `remove`, `grow-fs`
  (256 to 1224 MB while mounted read-only).

## Speech recognition

`lidar-stt` turns what is said after "Hey Jarvis" into a line on
`/run/lidar-assistant.fifo`. It runs with the `voice` service when the
`assistant` service is on and a speech model is installed (`lidar-models
install en|de`), and keeps the recognizer loaded: about 160 MB of RAM and
half a minute to load at boot.

- **Audio:** `lidar-tee` copies `lidar-voice`'s output into
  `/run/lidar-stt.pcm` without ever waiting for it, so the wake word never
  depends on recognition. Until a wake word, `lidar-stt` only keeps the
  last 6 s of audio in memory (no decoding).
- **Utterance:** recognition starts 0.15 s before the detection, so a
  command said straight after "Hey Jarvis" keeps its first word; a leading
  "Hey Jarvis" in the text is dropped. It ends after 0.8 s of silence
  after speech, or after 10 s; nothing said within 5 s ends it with no
  text. Empty results are dropped.
- **State:** `/run/lidar-stt.json`: `state` (`nomodel`, `loading`,
  `failed` (the model would not load: it waits for another one, without
  restarting), `idle`, `listening`, `decoding`), `lang`, `partial` (the
  text so far), `final` (the last line sent) and `seq`.
- **Ring:** once the text has reached the assistant, the ring pulses
  (`wake think`) until the answer is spoken; nothing said, or nobody to
  take the text, ends it (`wake off`), and so does an action that answers
  with nothing ("stop").
- **Try it** on a recording (16 kHz 16-bit mono):
  `lidar-stt -W 0 -o - -v < command.raw` treats the start as the wake word
  and prints the text.
- Another language installed: it loads the new model by itself.
- Measured on a PC (Piper "Hey Jarvis" + commands and LibriSpeech clips,
  through the real wake word detector, in real time): 39 of 40 English and
  6 of 6 German texts, 5 % word errors; the text arrives 1.3-3.1 s after
  the end of speech (median 2.0 s English, 2.9 s German). The recognizer
  decodes 1.28 s of audio at a time, so the 0.8 s endpoint is only seen at
  the end of one of those.

`tools/lidar-stt/build.sh <dir>` without a sysroot builds both tools for
the build host.

## Speech output

`lidar-tts` speaks the assistant's answers (and Claude's, sentence by
sentence) with the installed Piper voice, through the speaker at the
system volume. It runs with the `assistant` service; `lidar-assistant -y
lidar-say` hands each answer to it through `/run/lidar-tts.fifo` and goes
on at once.

- **Voice:** sherpa-onnx's VITS with the voice of `lidar-models`
  (`current/tts`, the settings of `current/info`). The phonemes come from
  espeak-ng, phonemized as Piper does it (clauses with their punctuation,
  decomposed, its vowel clusters merged): the same phoneme ids as Piper 1.8
  on 179 German and 166 English test texts. espeak-ng only phonemizes.
- **Cache:** each part's audio is kept in `/var/lib/lidar-models/current/cache`
  (20 MB, least recently used go first), keyed by voice, model, settings and
  text, and goes with the language. A release carries the cache of every
  voice (`lidar-voicecache-<voice>.tar`, made on the build host with
  `tools/mk-voicecache.sh`), which `lidar-models install` unpacks there.
  Without it (or for phrases added since), `lidar-tts` prepares the
  assistant's phrases and Claude's fixed sentences itself, at the lowest
  priority and only while it has nothing to say and nobody speaks to the
  Echo: 15-20 minutes on the Echo, shown as an orange arc on the ring and as
  `prepare` (`done`, `total`) in its state.
- **Speed on the Echo:** cached answers start within milliseconds;
  synthesis takes about 1.8 times as long as the speech (3 threads), so a
  Claude sentence of 3 s starts after about 5 s. The voice loads in about
  15 s and keeps about 200 MB of RAM.
- As the other sounds: silent while the speaker is muted, the sound display
  keeps off it, the LED ring shows `speak` while it talks. The wake word
  stops it at once.
- **State:** `/run/lidar-tts.json`: `state` (`nomodel`, `loading`, `idle`,
  `speaking`), `lang`, `queue`, and `prepare` while phrases are prepared.
  Status lines, never text, in the kernel log.
- **Try it:** `lidar-say de "Es ist sieben Uhr"`; `lidar-tts -I "<text>"`
  prints the phonemes and ids of a text and times its synthesis; `lidar-tts
  -B <dir>` renders the phrase cache into a directory (what
  `tools/mk-voicecache.sh de thorsten out.tar` does on a PC, in the build
  container: about 25 s).
