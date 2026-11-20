// SPDX-License-Identifier: CERN-OHL-S-2.0
// Parameters of the pogo-pin flash jig for the Echo Plus (2nd generation).
// pogo-jig.scad includes this file; every dimension of the jig is here.
//
// DEVICE FRAME, used for every coordinate below: looking at the bottom of
// the Echo from outside with the label text upright, x to the right, y up,
// z towards the viewer (out of the Echo, towards the table). Origin: centre
// of the threaded insert, on the bottom plate surface. The jig therefore
// occupies z >= 0, and the nose and the pins point towards -z.
//
// WINDOW FRAME (u, v): the device frame shifted to the pad-grid centre,
// u = x - window_x, v = y - window_y.

/* [Echo bottom] */
// Sources: "calipers" = measured on the unit; "photo" = scaled from a
// straight-on photo with the calipers in frame, good to about 0.2 mm.
plate_d = 73;            // flat bottom plate diameter (calipers; display only)
rim_w = 2;               // flat edge outside the plate (calipers; display only)
insert_relief_d = 14;    // relief around the insert, only cut if insert_relief_h > 0
insert_relief_h = 0;     // insert proud of the plate: flush (checked)
window_x = 24.4;         // window centre = pad grid centre, from the insert (photo)
window_y = 10.5;
window_w = 13.3;         // outer recess at the plate surface, along x (photo)
window_h = 10.6;         // outer recess, along y (photo)
window_r = 2.5;          // outer recess corner radius (photo)
window_depth = 1.95;     // plate surface to the ledge, the floor of the outer recess (calipers)
step_w = 10.0;           // inner pocket in that ledge, along x (calipers)
step_h = 7.0;            // inner pocket, along y (calipers)
step_depth = 2.0;        // ledge to the pocket floor, which has one hole per pad (calipers)
hole_d = 2.0;            // pad holes in the pocket floor (calipers; display only)
pad_d = 1.7;             // gold pad diameter (photo; display only)
pad_depth = 5.95;        // plate surface to the pad surface (calipers): decides the pin height
pitch_x = 3.0;           // pad pitch (calipers 3.0, photo 2.95 to 3.05; the grid is square)
pitch_y = 3.0;

/* [Pins] */
// Pad grid cells [column, row]: columns -1, 0, 1 left to right, rows 1 (top)
// and -1 (bottom). The 17 V pad and the cell diagonally opposite to it carry
// no pin and no hole, so no pin can reach 17 V even with the pin block
// turned by 180 degrees in the window.
pin_dp = [0, 1];         // TM4, USB D+
pin_dm = [1, 1];         // TM5, USB D-
pin_gnd = [0, -1];       // TM37, GND
pad_17v = [1, -1];       // TM33, VIN ~17 V: never a pin or a hole here
pin_d = 1.02;            // P75 barrel diameter
pin_len = 16.0;          // P75 free length (measured on the pins: 16). The pin heights depend on it
pin_stroke = 2.5;        // P75 full travel
pin_compress = 1.6;      // wanted compression on the pad: two thirds of the travel, the rated working point
pin_guide_d = 1.4;       // printed guide hole in the nose; ream to 1.1-1.2 if it prints tight
pin_guide_len = 3.0;     // length of the guide hole, from the nose end
pin_relief_d = 2.0;      // wider hole below the guide

/* [Base] */
// Printed with the face that lies on the Echo on the bed.
web_t = 2.4;             // flat plate on the Echo's bottom
hub_d = 18;              // boss around the M4 screw
post_d = 5.5;            // posts for the cap and for the cable clamp
screw_pilot_d = 1.7;     // pilot for M2 self-tapping screws
m2_clear_d = 2.2;        // clearance for M2 self-tapping screws
screw_head_h = 1.6;      // M2 pan head
bed_chamfer = 0.3;       // on the bed face, against elephant foot

/* [Pin block] */
// Separate part, printed nose down: a nose that fits the inner pocket and
// carries the pin guides, a looser boss in the outer recess, and a neck that
// sits in a conical seat of the base.
fit = 0.2;               // clearance per side, nose in the inner pocket: this fit locates the pins
window_fit = 0.4;        // clearance per side, boss in the outer recess (photo values, so loose)
nose_r = 1.5;            // nose corner radius; larger than the pocket's corners is safe
boss_gap = 0.2;          // boss and nose stay this far above the ledge and the pocket floor
boss_chamfer = 0.3;      // lead-in on the nose edge
neck_fit = 0.15;         // clearance per side, block in the base
block_shoulder = 1.2;    // 45 degree seat: height and width
block_gap = 0.1;         // the cone of the block clears its seat by this much while the nose is boss_gap above the pocket floor
jig_block_z1 = 4.0;      // the flange of the pin block ends here, measured from the plate surface
jig_play = 0.1;          // play of the pin block under the cap when its nose stands on the pocket floor
slot_w = 1.8;            // slots in the tower: pin 1.02 plus the solder joint
slot_depth = 3.0;        // the slots are only this deep, measured from the cap; below them the tower is a closed guide around each pin
tower_wall = 1.0;        // plastic around and between the slots

/* [Screw] */
m4_head = "countersunk"; // "countersunk" (DIN 7991 / ISO 10642, sits in a countersink) or "socket" (cap head on a washer)
m4_clear_d = 4.5;
m4_len = 10;             // nominal screw length: overall for a countersunk screw, under the head for a socket head
m4_engage = 4;           // thread length inside the insert; the insert is 5 mm deep (calipers)
m4_head_d = 8.0;         // countersunk head diameter (90 degree head)
m4_washer_t = 0.8;       // socket head only

/* [Cap, cable clamp, labels] */
// The pin block sits in the base and carries a tower that holds the three
// pins upright and apart; each pin has a slot in the tower for its soldered
// wire. A flat cap screwed to two posts of the base closes the tower and
// takes the spring force.
jig_posts = [[-8, 8.5], [8, -8.5]]; // cap screws (u, v): diagonal, within the Echo's flat bottom
cap_floor = 3.4;         // cap thickness; its screw heads are sunk, so its table side is flat: the Echo can stand on it
cable_d = 4.0;           // cable jacket; the clamp grips cables from about 3.6 to 4.6 mm with this value
cable_u = 0;             // the cable leaves along -v at this u
clamp_v = 24;            // cable clamp on the base, this far from the window centre: leaves about 20 mm of free wire between the pins and the jacket
clamp_w = 6;             // width of the clamp bar
clamp_bar_t = 3;         // clamp bar thickness
clamp_squeeze = 0.6;     // the bar presses the cable_d jacket flat by this much
label_size = 3.4;        // pin names on the base: bold, strokes about 0.75 mm wide for a 0.4 mm nozzle
label_depth = 0.8;       // engraved this deep

/* [Base with feet] */
// The base with two feet, so that the Echo can stand upright: it stands on
// the flat cap of the jig and on the two feet, which are as high as the cap
// and sit under the Echo's flat bottom. Printed like the plain base.
feet_angles = [120, 230]; // foot directions, measured from the direction of the window
feet_r = 34;             // foot radius: just inside the edge of the Echo's flat bottom
feet_d = 9;
feet_arm_w = 8;          // flat arm from the hub to a foot

/* [TPU soles, optional] */
// A shoe for the cap and a sock for each foot, printed in TPU: grip on the
// table, and nothing hard for the speaker to buzz against. All three are
// equally thick, so the jig stays level with or without them.
tpu_t = 1.2;             // sole thickness
tpu_wall = 1.2;          // wall of the shoe and the socks
tpu_lip = 2.4;           // how far the shoe reaches up the side of the cap
tpu_cup = 4.0;           // how far a sock reaches up its foot
tpu_fit = 0.1;           // the TPU parts are this much smaller per side than what they stretch over
