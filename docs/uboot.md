# U-Boot and the boot chain

[Documentation](README.md) · [Project README](../README.md)

## Boot chain

```
BootROM -> preloader (BOOT0) -> BL31 -> Amazon LK + Kaeru
        -> boot_a | boot_b  (Android boot image, unverified because unlocked)
             payload = U-Boot (this project) -> extlinux on system_a | system_b
             (or: payload = Linux Image.gz + DTB, booted by LK directly)
```

Nothing here writes BOOT0/BOOT1, `lk_*`, `tee*`, `expdb`, `kb` or `dkb`.
Kaeru's fastboot lives in partitions this project never touches, so it is
always available for recovery: hold **Action** while plugging in power.

### What LK requires of a boot image

Taken from a stock `boot_a` and from strings in LK:

| Field | Value |
|---|---|
| Header | Android boot image v0, page size 2048 |
| Kernel | 512-byte MediaTek `KERNEL` header (magic `0x58881688`) + gzip(arm64 Image) + appended DTBs |
| DTB selection | LK picks the appended DTB whose **first** root compatible matches the board exactly (`amzn,lidar-pvt` on retail units). It copies it to `0x4e000000` (max 0x80000 bytes), fixes up memory and cmdline, and passes it in x0 |
| Entry | 64-bit, EL2, at `0x40080000`. BL31 occupies `0x43000000` + 0x30000, so the Image plus BSS must end below that |
| Cmdline | must start with `bootopt=64S3,32N2,64N2`, or LK does not take the 64-bit path |
| Ramdisk | required; plain gzip, no MediaTek header (an empty cpio works) |

`tools/lidarboot.py` builds exactly this (`pack`) and takes images apart
(`unpack`). It checks the arm64 magic, the BL31 overlap, the DTB size and the
DTB compatible. A stock image round-trips byte-identically after
decompression.

## U-Boot behaviour

- U-Boot runs from wherever LK put it: it is position independent and carries
  an arm64 Image header. It does not reinitialise DRAM, PLLs or the CPU rail.
- The memory map stops at `0x7f9c0000`, so the TEE carve-out is never mapped.
- The watchdog is not auto-started.
- **Volume Up held at power-on** enters U-Boot USB fastboot on the pogo-pin
  USB. If no OS slot boots, U-Boot also ends up there. The boot0/boot1
  hardware partitions and the raw `mmc0` user area are not exposed. Named GPT
  partitions are, so do not flash `lk_*`, `tee*`, `expdb`, `kb` or `dkb`.
- **No UART input is needed.** In U-Boot fastboot,
  `fastboot oem run:<command>` runs any U-Boot command, and
  `fastboot oem console` reads back its output over USB. Autoboot has no
  delay, and only the exact string `lidar` would stop it, so noise on a
  floating RX pin cannot strand U-Boot at a prompt (`bootdelay 3` in the
  environment opens a window for UART work).
- The OS comes from `/boot/extlinux/extlinux.conf` on ext4 `system_${lidar_slot}`.
- A/B fallback uses `bootcount`, `bootlimit=3` and `upgrade_available`
  (verified on hardware: after three unconfirmed boots U-Boot switched
  `lidar_slot` and still came up through the other slot). They
  are stored in the environment at the top of `misc`: two 64 KiB copies at
  partition offsets `0x70000` and `0x60000`. Amazon only uses the first 6 KiB
  of `misc`. For `fw_setenv` in the OS, `/etc/fw_env.config` is:

  ```
  /dev/disk/by-partlabel/misc 0x70000 0x10000
  /dev/disk/by-partlabel/misc 0x60000 0x10000
  ```

  To update, write the inactive slot, then run
  `fw_setenv lidar_slot b; fw_setenv upgrade_available 1; fw_setenv bootcount 0`
  and reboot. Confirm the boot from userspace with
  `fw_setenv upgrade_available 0; fw_setenv bootcount 0`.

Useful commands from the PC:

```sh
fastboot oem run:"mmc info";         fastboot oem console
fastboot oem run:"part list mmc 0";  fastboot oem console
fastboot oem run:"button list";      fastboot oem console
```
