# Audio

[Documentation](README.md) · [Project README](../README.md)

## System volume

`lidar-volume` (the `volume` service) owns one speaker volume and mute, applied
to the codec's `PCM Playback Volume` in 0.5 dB steps up to the safety cap.
Volume 0 is silence: the codec's lowest step (-63.5 dB) is still audible, so
0 mutes like the mute flag (in `lidar-dsp`'s samples with the speaker tuning,
otherwise through the codec's DAC-to-driver switches). Everything that
changes volume feeds the same state:

- **Bluetooth phone** (`bluetooth` service): the phone's volume buttons and
  slider set the level, and changes made on the Echo move the phone's
  slider (see Bluetooth speaker).
- **Volume buttons:** 3 dB per press; holding repeats after 0.4 s, one step
  every 0.15 s. A short 520 Hz tone plays at the new level on the press and,
  after a hold, once at the final level, as on stock.
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
(-40 dB).

## Speaker tuning

Fire OS does not play audio flat: its audio library (`libasp.so`) runs every
speaker stream through a chain configured by
`/system/vendor/etc/audio-algorithms/AFE.cfg`. The `dsp` service runs the
same chain, reverse engineered from that library, on the owner's own copy of
these files (from `system_a` or lidar's copy of them, see
[Files from Fire OS](services.md#files-from-fire-os), at boot; nothing is redistributed):

1. the volume, applied digitally before everything else, as on stock;
2. a 2048-tap FIR equalizer chosen by the level (`EQ_50/90/100.cfg`): the
   speaker's response correction plus loudness compensation, +10 dB of bass
   up to -23.5 dB, +5 dB up to -4 dB, none above; a level change crosses
   zones with a 400 ms crossfade;
3. the parametric EQ (`ParametricEQ.cfg`: bass shelves +3 dB at 70 Hz and
   +9 dB at 110 Hz, -3 dB at 160 Hz);
4. the owner's tone settings, if any (`dsp bass`, `mid`, `treble`, ±6 dB:
   the three bands of stock's own user EQ, which Fire OS ships flat) and
   `dsp boost` (0 to 6 dB of gain);
5. the multiband compressor/limiter (`MBCL.cfg`): +6 dB in, four bands
   (100/220/5000 Hz Linkwitz-Riley crossovers), a slow RMS compressor and a
   2 ms look-ahead peak limiter per band, a full-band limiter, a hard clip;
6. both channels mixed to mono for the single speaker.

So with the service on, the same volume percentage is noticeably louder and
fuller than without, as on stock, and the compressor only works hard near
full volume. The limiters keep the output below digital full scale; the
safety cap still applies, since the codec sits at the cap and the volume is
taken off digitally. The line-out jack gets the same tuned mono signal
(stock bypasses the chain for external outputs).

Tone and boost sit in front of the compressor/limiter, so whatever is set,
every band and the sum stay below full scale: more bass or boost makes the
compressors work harder instead of the speaker. Near full volume a boost
therefore adds little; at low and middle volumes it is the full 6 dB.
`lidar-feature set dsp bass 3` applies at once while the tuning runs
(`lidar-dsp` re-reads the settings on SIGHUP and crossfades to them in 20
ms, without clicks), otherwise when it next starts; a tuning without a
multiband compressor leaves tone and boost off, since nothing would limit
them. With all four at 0 the output is bit for bit that of the tuning
alone. Verified
on the PC: response as set, output identical with all at 0, peaks capped at
full scale with everything at +6, no step larger than the signal's own on a
change.

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

## Bluetooth speaker

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
  the ring), and the volume buttons and the web interface move the phone's
  slider. The phone answers a change with its own rounding of it (an iPhone
  has 16 steps); `lidar-volume` takes what comes back from the phone within
  1.5 s of its own write as that echo and ignores it. A real change on the
  phone inside those 1.5 s is lost the same way; the next one counts again.
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

## Sound display

With the `visualizer` extra, the LED ring shows what the speaker plays
(`lidar-viz`). It listens to the AFE's loopback of the DAC input
(`hw:amazonlidar,2`), so every source counts: Bluetooth, the web player,
system sounds. 40 frames per second.

The loopback is taken after the speaker tuning and before the codec: with
`dsp`, `lidar-dsp` has already applied the system volume (up to the cap);
on the direct path the codec applies it afterwards. `lidar-viz` adds what
is missing from `/run/lidar-volume.state`, so both paths look the same.

- `level` (default) shows the last 43 ms against a reference that follows
  the loudest recent level and sinks 6 dB in 4 s: at the reference the arc
  is full, 18 dB below it dark. Music after the speaker tuning's
  compressor varies only a few dB around its average, so a fixed scale
  barely moved; this way every beat shows, at any volume. The arc rises
  within a few frames and falls back over about 0.3 s, so it swings with
  the beat rather than flickering. Each frame first reads whatever audio
  piled up, so the display is never behind.
- `spectrum` runs a 2048-point FFT and measures each band against its own
  reference level, which follows the loudest recent level and halves in
  4 s, so it looks the same at any volume.
- The ring service's own displays win: while `lidar-ring` shows the privacy
  red, the volume arc, pairing or the wake overlay it holds `/run/lidar-ring.busy`, and
  `lidar-viz` keeps off the ring. Brightness follows the light sensor as the
  rest of the ring (`/run/lidar-ring.level`).
- Dark while the speaker is muted, at volume 0, and while nothing plays
  (silence is judged before the volume). Dark too while one of our own
  sounds plays (volume tone, Bluetooth sounds, the wake chime, a ringing timer or alarm):
  whatever plays one writes the time it ends, in seconds since boot, to
  `/run/lidar-viz.quiet` (the later end wins).
- Verified on hardware: both styles with music over Bluetooth, the volume
  arc, mute, privacy red and pairing taking the ring and handing it back.
