# Flash jig: measurements and test fit

Every value below is a single line in `params.scad`. After changing one, run
`python build.py` (the model asserts on impossible combinations).

Views: "device view" means looking at the bottom of the Echo from outside
with the label text upright; x to the right, y up, origin at the centre of
the threaded insert.

Sections 1 to 4 are done on the **unpowered** device. Section 5 needs power.

## 1. Measurements

Taken on one unit. "Photo" values are scaled from a straight-on photo with
the calipers in frame and are good to about 0.2 mm; the test fit in
section 3 checks them.

| What | Value | Source | Parameter |
|---|---|---|---|
| Pad pitch, both ways | 3.0 mm | calipers (hole rows 8 and 5 mm over 2 mm holes); photo 2.95 to 3.05 | `pitch_x`, `pitch_y` |
| Bottom surface to the ledge (outer recess floor) | 1.95 mm | calipers | `window_depth` |
| Ledge to the floor of the inner pocket | 2.0 mm | calipers | `step_depth` |
| Bottom surface to a pad | 5.95 mm | calipers | `pad_depth` |
| Inner pocket (the one with the six holes) | 10 x 7 mm | calipers | `step_w`, `step_h` |
| Pad holes | 2.0 mm, countersunk to about 2.7 | calipers, photo | `hole_d` |
| Pads | 1.7 mm | photo | `pad_d` |
| Outer recess at the surface | 13.3 x 10.6 mm, corners about 2.5 mm | photo | `window_w`, `window_h`, `window_r` |
| Window centre from the insert | x +24.4, y +10.5 mm | photo | `window_x`, `window_y` |
| Bottom plate, rim, insert | flush with each other | checked | `insert_relief_h` |
| Insert depth | 5 mm | calipers | `m4_engage` = 4 |
| Pogo pins | 16 mm long | measured | `pin_len` |
| 17 V pad | bottom right, on both units checked | multimeter | `pad_17v` |

The outer recess, the inner pocket, the holes and the pads are concentric.
In a photo they look shifted against each other because they lie at
different depths.

## 2. Which print depends on which value

| Changed | Reprint |
|---|---|
| `step_w`, `step_h`, `fit`, `step_depth`, `window_depth`, `pitch_*` | pin block |
| `window_w`, `window_h`, `window_r`, `window_fit` | pin block and base |
| `window_x`, `window_y`, `m4_*` | base |
| `pad_depth`, `pin_len`, `pin_compress` | pin block and base |
| `cable_d` | base and clamp bar |

## 3. Test fit (print `jig-pin-block.stl` and `jig-base.stl` first)

Echo unpowered, upside down.

- [ ] 3.1 Pin block alone, nose first into the window: the nose drops into
      the inner pocket with little play (0.2 to 0.3 mm) and goes all the way
      down: the wide flange of the block then stands 3.8 mm above the bottom
      surface. Too tight or too loose: `step_w`, `step_h` or `fit`. Stops
      early: the boss does not fit the outer recess (`window_w`, `window_h`,
      `window_fit`).
- [ ] 3.2 With the block in, look straight down each of the three holes with
      a light: a gold pad fills the hole, centred. All three off the same
      way: note how far and in which direction. Only the right-hand one
      off: pitch.
- [ ] 3.3 There is no hole over the bottom right (17 V) pad and none over
      the top left pad.
- [ ] 3.4 Base on the bottom, flat face down, M4 screw in loosely. It lies
      flat and does not rock.
- [ ] 3.5 Drop the pin block through the base into the window. It must go
      in and sit down in the cone without the base having to be pushed
      sideways against the screw. If it does not, note how far the opening
      is off and in which direction (`window_x`, `window_y`).
- [ ] 3.6 Tighten the screw: it clamps the base before it bottoms out.
      Bottoms out first: reduce `m4_engage`.
- [ ] 3.7 Pad depth: with the block seated in the base, drop a pogo pin, tip
      first, into a hole. It rests on the pad without being compressed.
      Measure how far its end stands above the top of the tower: P.
      `pad_depth` = pin length - P - 8.55. With 5.95 mm and a 16.0 mm pin,
      P is 1.5 mm. The jig is right when P is between 1.3 and 1.8 mm.

## 4. Jig, before power (README.md for the steps)

- [ ] 4.1 Pins with the wires soldered on, out of the jig: each joint lies
      within 2.5 mm of the pin's closed end, nothing sticks out past the end
      face, each pin with its joint drops into its slot of the tower, and
      each plunger still springs back over its full travel. The red wire
      and the shield are cut off at the jacket.
- [ ] 4.2 Multimeter, continuity: USB plug GND contact to the tip of the
      black wire's pin, D+ contact to the green one's, D- contact to the
      white one's: beep. Between any two pins: no beep. VBUS contact to any
      pin: no beep.
- [ ] 4.3 Jig assembled on the Echo: the cap lies flat on both posts with
      the screws snug. If the pins hold it off the posts, the pad depth is
      smaller than measured (3.7). If the tower of the pin block holds it
      off, raise `jig_play`.
- [ ] 4.4 Cable clamp: the bar sits on both of its posts and the cable
      cannot be pulled through by hand. If it slips, your cable is thinner
      than about 3.6 mm: set `cable_d` to its diameter and reprint the base
      and the bar.
- [ ] 4.5 Continuity from the GND contact of the USB plug to the Echo's
      ground (outer barrel of its DC jack): beep, so the GND pin makes
      contact at both ends.

## 5. First connection (device powered)

- [ ] 5.1 Echo powered, jig mounted, USB plug **not** in the PC. Multimeter,
      DC volts, black probe on the plug's GND contact: D+ and D- contacts
      between 0 and 3.6 V, VBUS contact 0 V. Anything higher: unplug the
      Echo's power and stop.
- [ ] 5.2 Connect to the PC, preferably through a hub you can afford to
      lose for the first try. Expect the ACM serial port and the NCM
      interface (device at 169.254.7.2), or U-Boot fastboot.
- [ ] 5.3 If it enumerates and drops, or not at all: re-seat the jig, check
      4.2 and 4.5 again, then try through a USB 1.1 hub or a full-speed
      isolator (forces 12 Mbit/s).
- [ ] 5.4 A large fastboot download or a few minutes of serial traffic
      without a disconnect in the host's log.
