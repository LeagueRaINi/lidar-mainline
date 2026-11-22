# Install

[Documentation](README.md) · [Project README](../README.md)

## Prerequisites

- An Echo Plus 2nd gen that is already unlocked and runs Kaeru LK.
- Your own full eMMC dump from the unlock, at least `boot_a`, for recovery.
- `fastboot` (Android platform-tools) and `podman` on the host.
- Network access while building: the test image downloads Alpine packages.
- Access to the USB test points (TM4, TM5 and TM37, see
  [Test points](hardware.md#test-points)), for example with pogo pins;
  [`pogo-jig/`](../pogo-jig/) is a printable jig for that. All flashing, the
  shell and the network go over this USB link.
- Optional: a 1.8 V UART adapter on TM58 (UART0 TX, 921600 8N1), output only.

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
   host's interface any other link-local address. If the `telnet` extra is
   on (an image you build yourself enables it, a release does not), a
   second root shell without authentication listens on
   `telnet 169.254.7.2` for when the serial shell
   is busy. By default it only accepts connections arriving on `usb0`; see
   `telnet listen` under [Services](services.md).

**Previous boot's log:** after a warm reboot or a kernel panic (the kernel
reboots 10 s after a panic), `/sys/fs/pstore/` holds the previous boot's
console (`console-ramoops-0`) and crash report (`dmesg-ramoops-0`). The log
lives in a reserved RAM region at `0x60000000`, below U-Boot's fastboot
download buffer (`0x60100000`) and outside its kernel and initramfs load
area.

**Recovery:** hold **Action** while plugging in, then
`fastboot flash boot_a <your boot_a dump>` restores the stock boot path.

## Browser installer

`installer/install.html` (published at
<https://leagueraini.github.io/lidar-mainline/install.html>, linked from the
project page there) is a static page that does steps 1 and 2 from a desktop
Chromium browser (Chrome, Edge, Brave, Opera) over WebUSB. It needs what
the manual way needs: Kaeru fastboot already on the device, your own
`boot_a` dump, and the pogo-pin USB. It contains no unlock code.

- One click on "Download and use" fetches the latest published release's
  two images and checks them. A page cannot fetch GitHub release assets
  (their download URLs redirect to a host without CORS headers, tested),
  so the Pages workflow (`.github/workflows/pages.yml`, run again for every
  published release) puts the release's images, gzipped (about 37 MB), and
  `release.json` with their SHA-256 next to the page; the page unpacks them
  and compares them with that and with the `digest` the GitHub API gives
  for each release file. Files you download or build yourself can still
  be picked: they are checked against the release, or against a
  `SHA256SUMS` you pick, and otherwise flashed as "not verified" after a
  warning. Nothing is uploaded.
- For each phase a photo of the Echo's top shows the button to hold,
  circled: Action for Kaeru fastboot, Volume Up for U-Boot's.
- It writes `boot_a` and `system_b` only and refuses any other partition.
  If your Fire OS runs from slot b, install by hand as above.
- Before each flash it checks which fastboot answers (Kaeru's for `boot_a`,
  U-Boot's for `system_b`) and asks for confirmation. It does not read the
  serial number.
- Windows: the fastboot device must use the WinUSB driver; the page explains
  how. Linux may need a udev rule for USB ID `0bb4:0c01`.
- To run it locally: `python -m http.server` in `installer/`, then open
  `http://localhost:8000/install.html` (WebUSB needs https or localhost, and the page
  does not work from `file://`).
- **Not yet tried on an Echo.** Both phases, wrong-mode and foreign
  devices, a failed flash and a lost USB connection were only run against
  a simulated fastboot device in the browser. Open on hardware: the Windows
  driver steps, the udev rule, how Kaeru's fastboot answers the page's
  `getvar` queries, whether U-Boot accepts the page's sparse pieces, and
  the GitHub Pages workflow.

USB access uses [fastboot.js](https://github.com/kdrag0n/fastboot.js) v1.1.3
(MIT), vendored unmodified in `installer/vendor/` with its licence and
checksum.
