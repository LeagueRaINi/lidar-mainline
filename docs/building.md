# Building

[Documentation](README.md) · [Project README](../README.md)

Run from the repository root (bash on Linux, or Git Bash on Windows):

```sh
podman build -t lidar-build -f Containerfile .
podman volume create lidar-src
podman run --rm -v lidar-src:/src lidar-build bash -c '
  git clone --depth 1 -b v7.3 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git /src/linux &&
  git clone --depth 1 -b v2026.10 https://source.denx.de/u-boot/u-boot.git /src/u-boot'

W="$(pwd -W 2>/dev/null || pwd)"
run() { podman run --rm -v lidar-src:/src -v "$W:/work" lidar-build bash /work/build.sh "$@"; }
run uboot               # -> out/lidar-uboot.img
run linux lidar         # Image.gz, dtbs, modules in /src/build/lidar
# The zigbee and thread services need the EFR32's multi-PAN RCP and zigbeed,
# built in their own container (Silicon Labs' toolchain; see efr32/README.md)
# before testimg picks them up:
podman build -t efr-build -f efr32/Containerfile efr32
podman run --rm -v efr-src:/efr -v "$W:/work" efr-build bash /work/efr32/build.sh    # -> out/efr32/lidar-mp-rcp.gbl
podman run --rm -v efr-src:/efr -v "$W:/work" efr-build bash /work/efr32/zigbeed.sh  # -> out/zigbeed/zigbeed
run testimg lidar       # -> out/lidar-test-system.ext4 (also builds cpcd and ot-br-posix)
run bootimg lidar       # -> out/lidar-linux-boot.img (kernel booted straight from LK)
run patches             # -> out/patches/{linux,u-boot}-lidar.diff
```

The kernel boots without `clk_ignore_unused`: it gates every clock that is
on but unclaimed at the end of boot (the NFI, SPI-NOR, BSI, AES, SEJ, RBIST,
MSDC1 and debug gates, the APLL1 dividers, `memslp_dlyer` and `apxgpt`).
Tested on hardware with these gated: eMMC, I2C, microphones, the speaker (by
ear, with and without the speaker tuning), USB, LEDs, the light sensor,
Wi-Fi, Bluetooth and the EFR32 UART.

## Releases

A tag `vMAJOR.MINOR.PATCH` (annotated; its message becomes the release
notes) makes `.github/workflows/release.yml` build `lidar-uboot.img` and
`lidar-test-system.ext4` in the container and open a draft release with
them, `SHA256SUMS` and `lidar-release.txt` (kernel, Alpine packages). A
suffix (`v1.3.0-rc.1`) marks a pre-release. The image reports its version
in `/etc/lidar-release` (`local` for your own builds), and the web
interface shows it.

Release images differ from the ones you build yourself:

- No Silicon Labs software (`LIDAR_NO_SILABS=1`), so no Zigbee and no
  Thread: build the image yourself for those ([`efr32/`](../efr32/)).
- The `telnet` extra is off.

`tools/mk-release.sh` checks the image before a release is made and
refuses one with MSLA software, Fire OS files, settings or telnet in it.
The sources of the Alpine packages listed in `lidar-release.txt` are in
Alpine's [aports](https://gitlab.alpinelinux.org/alpine/aports) tree.

**Pins.** One commit builds the same images every time, or not at all:
Linux and U-Boot by tag (`build.sh`), everything else the build downloads
by commit or SHA-256 (`tools/mk-testimg.sh`, `tools/mk-wakeword.sh`), the
Alpine release and each package with version and SHA-256 in
`tools/alpine.lock`. Alpine serves only the newest build of a package, so
the lock goes stale when Alpine updates one, and the build then stops and
names it. To bump, or after adding a package to `mk-testimg.sh`:

```sh
podman run --rm -v "$PWD:/work" lidar-build python3 /work/tools/alpine-fetch.py --update [new-package...]
```

and commit the lock. For an older tag whose packages Alpine no longer
serves, use that release's images.
