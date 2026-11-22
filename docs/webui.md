# Web interface

[Documentation](README.md) · [Project README](../README.md)

With the `webui` service, `http://169.254.7.2/` on the USB host shows the
Echo's state, plays music on it and holds every setting, also without
Wi-Fi or Bluetooth set up. It is one page without anything loaded from
elsewhere, with a light and a dark theme, a side bar on wide screens and,
on a phone, four shortcuts at the bottom and the whole menu under "More".
Each service switch and each setting sits in the category it belongs to:

- **Overview:** the LED ring as it is lit, the system volume and mute
  (`lidar-volume`), what plays, the privacy switch, whether the speaker
  tuning runs, Wi-Fi and Bluetooth at a glance, CPU load, memory, the three
  board temperature sensors, the clock, the light sensor, USB traffic and
  the supply rails.
- **Music:** songs are dropped onto the page; the browser decodes them (any
  format it can play), converts them to 48 kHz 16-bit stereo and uploads
  them. `lidar-player` plays the library in order, with pause, seek and
  skip. The library lives in RAM (`webui storage_mb`) and is gone after a
  reboot.
- **Sound:** the volume and its settings (`volume initial`,
  `max_attenuation`, `sound`, `keepalive`), the tone controls as sliders
  (`dsp bass`, `mid`, `treble`, `boost`, heard while they move; "Flat" puts
  bass, mids and treble back to 0) and the `volume` and `dsp` services.
- **Voice:** the `assistant` and `voice` services, wake word, sensitivity,
  language (and the German voice with German), the Claude API key
  (write-only, with a test), the time zone (common ones by city, any other
  as a POSIX TZ string) and the alarm volume. "Listening" draws the Echo
  from above, five times a second while the view is open: Volume down at
  the top, the microphone button on the right, Volume up at the bottom,
  Action on the left, and around it the twelve beams of `lidar-voice` as
  bars (how far each is over the noise), the one in use coloured; the ring
  lights up towards a wake word when one is heard. "Type to the assistant"
  sends a line as if it had been spoken and lists what was understood and
  answered; below it, what rings, the timers with their time left and the
  alarms.
- **Light ring:** the ring as it is lit, the `ring`, `visualizer` and
  `bootanim` services, brightness (`ring auto_brightness`,
  `min_brightness`), `visualizer style` and the volume arc (`volume ring`).
- **Wi-Fi:** status (network, signal, band, bit rate, addresses), a scan
  list to pick a network from, connect (saved as `wifi ssid` and `wifi
  passphrase` and joined at once, through `lidar-wifi`) and forget, the
  `wifi` service and `wifi country`.
- **Bluetooth:** with the `bluetooth` service (it loads the Bluetooth
  driver): power, the speaker's state, pairing mode on and off, the paired
  phones with a button to forget each, a scan of nearby devices (name,
  address, type, signal) through `btmgmt`, and the `bluetooth` settings.
- **Smart home:** the `zigbee` and `thread` services with their settings
  and state (Thread: role, network, channel, PAN ID, border routing).
- **Security:** the login (below), `webui listen`, and `telnet` with
  `telnet listen`. Switching `telnet` on asks first: it is a root shell
  without a password for everyone on the USB network.
- **System:** release and kernel, reboot, "Run setup again", the `webui`
  service (switching it off asks first) and `webui storage_mb`, the files
  from Fire OS with backup and restore, and the download of the Fire OS
  image.
- **Debug:** the kernel log with a filter and live follow, the previous
  boot's log from pstore (after a crash or a warm reboot), processes,
  storage, mounts and modules.
- **Files:** a read-only file browser with text preview (first 256 KB) and
  download. Device nodes, `/proc/kmsg`, `/proc/kcore`, process memory and
  debugfs are refused: reading some of them blocks, and debugfs register
  dumps of clock-gated blocks can stall the bus. `lidar.conf` on the
  booted slot and its copy from boot (`/run/lidar.conf.boot`) are refused
  too, because they hold the Wi-Fi passphrase, the API key and the login's
  hash. Everything else is served.

A switch, slider or choice is sent the moment it is used, and its row says
what became of it. What waits for a reboot is the Echo's to say
(`lidar-feature pending`): while that list holds anything, a bar with
"Reboot now" stays at the top and the rows in it say "after reboot"; a
change taken back leaves the list again, and after a reboot it is empty.
The page asks for it when a view opens, after every change and when the
Echo is back. In the API, `set`, `unset`, `enable` and `disable` answer
`{"ok": true, "reboot": true|false, "pending": <how many wait>, "message":
"<what lidar-feature printed>"}`, and `pending` answers `{"pending":
["feature.<service>", "<feature>.<key>", ...]}`. The controls follow what
`lidar-feature` allows (`on|off` a switch, a range a slider, a few values
buttons, more a list). A service or setting the page has no place for yet,
from a newer image, shows under "Advanced" in the category of its service,
or under System, so none is ever missing. Over Wi-Fi the switches show but
are disabled, and the settings are not sent (see Login below).

For the Voice view the API has `voice`, which answers `{"voice": ...,
"wake": ..., "stt": ..., "tts": ..., "assistant": ...}` with the contents
of `/run/lidar-voice.json`, `/run/lidar-wake.json`, `/run/lidar-stt.json`,
`/run/lidar-tts.json` and `/run/lidar-assistant.json` (`null` for one that
is not there), and `assistant_say` (POST, `application/octet-stream`, one
line of at most 300 bytes): the line goes to `/run/lidar-assistant.fifo`,
and the answer, after at most 3 s, is `{"answered": true|false,
"assistant": <its state>}`, whose `last` is the answer to that line if
`answered` is true. Both are for the USB network only. `stats` carries
`tts` too: while its `prepare` is there, every view shows a banner that
the voice is preparing its phrases and keeps the CPU busy.

## Language

The web interface is in English and German. Without a choice
it follows the browser (the first German or English entry in its preferred
languages, English otherwise); the EN/DE switch in the side bar (on a
phone: next to the theme button at the top, and on the login form) changes
it at once and is remembered in the browser. Names, explanations and value
names of services and settings are part of the page (`feature.<name>`,
`<feature>.<key>`); one from a newer image without a German text shows
with the device's own English description. Known messages from the device
are shown in German too, any other as the device sent it. Values the
device needs, logs and file contents are never translated.

## First-run setup

Until it has been finished once (`webui setup_done`),
the web interface opens a setup for clients on the USB network; "Run setup
again" under System starts it later. Every step can be skipped.

1. *Protect the web interface*: a PIN or password for the web interface,
   and of whom it is asked (see Login below). Skipping it leaves the web
   interface open.
2. *Fire OS files*: shows whether lidar's copy of the Wi-Fi firmware and
   the speaker tuning is complete, and restores a `lidar-fireos export`
   backup if not (see [Files from Fire OS](services.md#files-from-fire-os)).
3. *Fire OS image*: downloads the whole Fire OS partition (`system_a`) as
   `fireos-system_a.img.gz`, offered when lidar booted from slot b and
   Fire OS is still on `system_a`. The partition is only read, never
   mounted. The image is private: keep it, never share or publish it.
   Ticking "I saved the file" sets `webui fireos_image_saved`.
4. *Wi-Fi*: scan and join, as under Wi-Fi.
5. *Device name*: the name phones show for the Bluetooth speaker
   (`bluetooth name`), after a reboot. It is not a network hostname.
6. *Voice assistant*: wake word, language, sensitivity and the Claude API
   key (the `voice` settings). The web interface never returns the key,
   only whether one is set and its last four characters. "Test key" asks
   the Anthropic API for its model list from the Echo, over TLS with the
   key on `socat`'s standard input; it needs Wi-Fi and a set clock. The
   assistant uses them with speech recognition and speech output (see
   [Speech recognition](voice.md#speech-recognition) and
   [Speech output](voice.md#speech-output)) once a speech model is installed
   (`lidar-models install`, about 120 MB over Wi-Fi), and Claude when a key
   is set.

The setup adds these to the API: `setup` (finished or not, whether this
client may run it, and the state of the login; answered for everyone),
`fireos_image` (the gzip stream), `voice_key` (set or not, last four
characters) and `voice_key_test` (`ok`, `invalid`, `offline` or `error`);
`fireos` also reports `image_bytes`, the partition size.

Verified on hardware, through the API: `setup`, `voice_key`, setting and
removing the key, `voice_key_test` with a made-up key (`invalid`, so the
TLS connection and the certificate check work), the `fireos_image` stream
(about 6 MB/s of partition data, a valid gzip file) and the refusal of
`lidar.conf`. A key test with a valid key has not been run, and the pages
of the setup were walked through in a desktop browser against a mock
backend, not against the Echo.

## Login

The owner can protect the web interface with one PIN (digits
only, at least 6) or one password (at least 10 printable ASCII characters);
there are no user names. It is set in the first step of the setup and
changed or removed under Security, Login, and `webui auth` says of whom it is
asked:

- `off` (the default until one is set): no login.
- `wifi`: clients that are not on the USB network have to log in; the USB
  network stays open as before.
- `all`: clients on the USB network have to log in too.

A client that has to log in gets nothing but the login form: every request
to the API is refused without a session, downloads included, except `setup`
and `login`. A PIN is fine against people in the same room; where the web
interface is open to the Wi-Fi (`webui listen any`), prefer a password, and
the page says so when a PIN is chosen there.

- **What is stored.** Only a sha512-crypt hash with a random salt
  (`cryptpw -m sha512`) in `webui auth_hash`, in `lidar.conf`, which only
  root can read and the file browser refuses. The PIN or password reaches
  `cryptpw` on standard input and is never an argument, a log line or a
  file. A login is checked by hashing it again with the stored salt.
- **Sessions.** A login puts a random token (32 bytes from `/dev/urandom`)
  into the cookie `lidar_session` (`HttpOnly`, `SameSite=Strict`), good for
  30 days. The Echo keeps only its SHA-256, under `/run/lidar-webauth`, so
  every browser logs in again after a reboot. "Log out" ends the session;
  changing the PIN, the password or `webui auth` ends all of them, and the
  browser that made the change stays logged in.
- **Wrong attempts.** Failed logins are counted per client address. From
  the 5th in a row on, each one locks that address: 10 minutes, then 30
  minutes every time; the answer says how many seconds are left,
  and the page counts them down. A right login resets the count. All
  failures from outside the USB network are counted together as well and
  lock every such client in the same way from the 20th in a row, so
  changing the address does not help. The USB network is not held up by
  that count: nobody on the Wi-Fi can lock the owner out of it. The counts
  are gone with a reboot.
- **What a login does not do.** It does not make a client admin: with or
  without it, everything that changes the device or reads more than its
  status (Wi-Fi and Bluetooth control, services and settings, logs, files,
  reboot, the login itself) is only answered for clients on the USB network
  (`169.254.0.0/16`). Over Wi-Fi a logged-in client gets the dashboard, the
  volume, music and the network status, as every Wi-Fi client does without
  a login set. Changing or removing the login needs a session or the
  current PIN or password.
- **Forgotten.** Restart the Echo and, once the ring lights up (the boot
  animation; not while plugging in, where Volume up alone enters U-Boot's
  fastboot), hold **Volume up** and **Volume down** together until the
  ring flashes green three times, about 10 seconds; start within the first
  30 seconds. That removes `webui auth`, `auth_hash` and `auth_kind` and
  ends every session; the kernel log has a line. The volume may shift a
  little meanwhile. Not the
  microphone button: every press toggles its hardware mute latch. Whoever
  can touch the Echo can do this; the login protects against the network,
  not against hands. On a shell, `lidar-feature unset webui auth` turns the
  login off.
- **No TLS.** The web interface is plain HTTP: the PIN or password, the
  session cookie and everything else cross the network unencrypted, and
  whoever can listen in on the Wi-Fi can read them. Use it in your own
  network only. Never forward a port to it or otherwise open it to the
  internet; for access from outside, use a VPN into the home network.

The login adds `login` (body: the PIN or password; sets the cookie, or
answers `{"error": ..., "wait": <seconds>}`), `logout` and `auth_set`
(body, one per line: `off`, `wifi` or `all`; `pin` or `password`; the
current PIN or password, needed unless none is set or the browser is logged
in; the new one, or empty to keep it) to the API. A request that needs a
login gets `{"error": "login required", "login": true}`. `set` and `unset`
refuse the three `webui auth` settings. For scripts: log in once with
`curl -c cookies.txt`, then pass `-b cookies.txt`.

Verified on the Echo: setting a PIN, every request and download refused
without a session, the cookie (`HttpOnly; SameSite=Strict`, 30 days), the
lockout at the 5th wrong PIN (with the earlier 60 s step) refusing even the
right PIN until it ended, logout, removing the login, and the button reset
with both volume buttons (three green flashes, login gone). One check of a
PIN takes about 0.3 s. Not yet tried on the Echo: the 10 minute lockout
step itself (the same code path with a longer time).

## Network access and API

As with telnet, the server (BusyBox `httpd` behind `lidar-listen`) only
accepts connections that arrive on `usb0` unless `webui listen any` is set.
Unlike telnet, a web server can also be reached by other web pages open in
the host's browser, so the API only takes commands
as POST with `Content-Type: application/json`, or `application/octet-stream`
for those that carry data (uploads, Wi-Fi credentials, setting values, the
login), which a foreign page cannot send without a CORS preflight that the
server never answers, and only when the Echo is addressed by IP (no DNS
rebinding). Everything on the page comes from `/cgi-bin/api` as JSON, so
scripts can use it too:

```sh
curl 'http://169.254.7.2/cgi-bin/api?stats'
curl 'http://169.254.7.2/cgi-bin/api?network'
curl -X POST -H 'Content-Type: application/json' -d '{}' 'http://169.254.7.2/cgi-bin/api?volume=40'
printf 'MyNetwork\nmy passphrase' | curl -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @- 'http://169.254.7.2/cgi-bin/api?wifi_connect'
printf '246813' | curl -c cookies.txt -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @- 'http://169.254.7.2/cgi-bin/api?login'
curl -b cookies.txt 'http://169.254.7.2/cgi-bin/api?stats'
```
