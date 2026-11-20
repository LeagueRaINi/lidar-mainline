// SPDX-License-Identifier: CERN-OHL-S-2.0
// Pogo-pin flash jig for the debug USB of the Echo Plus (2nd generation).
//
// A pin block carries a nose that locates in the recessed test-pad window
// and guides three pogo pins (USB D+, D-, GND) onto the pads. There is no
// hole over the 17 V pad. The block sits in a base that is fixed by one M4
// screw into the centre insert; a cap on two posts of the base holds the
// pin ends. No PCB: the wires of a USB cable are soldered straight to the
// pins and held by a clamp on the base. With the plain base the Echo lies
// upside down; on the base with feet it stands upright, on the cap and the
// two feet.
//
// Every part prints without supports, as exported.
//
// All dimensions are in params.scad. The model is built in the device
// frame described there (x right, y up when looking at the Echo's bottom,
// z out of the Echo): the parts occupy z >= 0, the nose points to -z.
// Do not mirror the model; "flip" only rotates it for display.

include <params.scad>

/* [View] */
part = "assembly"; // [base, base_feet, block, cap, clamp, tpu_shoe, tpu_sock, assembly, section_pins, exploded]
// display only: the base with feet, with the TPU soles on
on_feet = false;
// display only: Echo side up (a rotation, not a mirror)
flip = true;
// display only: draw the Echo's bottom and compress the pins onto the pads
show_echo = true;

/* [Hidden] */
$fn = 96;
eps = 0.01;

// ---------------------------------------------------------------- derived

boss_h = window_depth - boss_gap;             // boss in the outer recess
nose_z = window_depth + step_depth - boss_gap; // nose end, in the inner pocket
// hub height. A countersunk head ends 0.3 below the hub top; a socket head sits on a washer on top of it.
csk = m4_head == "countersunk";
csk_d = m4_head_d + 0.6;                      // countersink diameter at the hub top
csk_depth = (csk_d - m4_clear_d) / 2;
screw_wall = csk ? m4_len - m4_engage + 0.3 : m4_len - m4_engage - m4_washer_t;
tip_depth = pad_depth + pin_compress;         // a free pin tip ends this far below the plate surface
block_z0 = web_t + block_gap;                 // flange of the pin block, Echo side
window_a = atan2(window_y, window_x);

// a flat cap on two posts closes the tower of the pin block
jig_top_z = pin_len - tip_depth;              // cap face on the Echo side = pin end when the tip is free
jig_bot_z = jig_top_z + cap_floor;            // cap face on the table side
jig_plate_u = window_w / 2 + 3.15;            // half width of the base plate around the seat
tower_top_z = jig_top_z + boss_gap - jig_play; // the block drops by boss_gap until its nose stands on the pocket floor
// cable clamp on the base: a cradle and two posts, and a bar screwed onto the posts
clamp_u = cable_d / 2 + 3.2;                  // posts left and right of the cable
clamp_floor_z = web_t + 0.8;                  // bottom of the cradle groove
clamp_post_z = clamp_floor_z + cable_d - clamp_squeeze; // post tops = underside of the bar
cable_z = clamp_floor_z + cable_d / 2;        // cable axis
// direction in which the wire of each pin (D+, D-, GND) leaves the tower: away from the other pins
pin_exits = [[-1, 0], [1, 0], [0, -1]];

pins = [pin_dp, pin_dm, pin_gnd];
all_cells = [for (c = [-1, 0, 1], r = [1, -1]) [c, r]];

function cell(c) = [window_x + c[0] * pitch_x, window_y + c[1] * pitch_y / 2];

// footprint of the tower around the three pins
tower_m = slot_w / 2 + tower_wall;            // pin axis to the tower edge
tower_lo = [min([for (p = pins) cell(p)[0]]) - tower_m, min([for (p = pins) cell(p)[1]]) - tower_m];
tower_hi = [max([for (p = pins) cell(p)[0]]) + tower_m, max([for (p = pins) cell(p)[1]]) + tower_m];

// ------------------------------------------------------------ sanity checks

for (p = pins) {
    assert(p != pad_17v, "a pin sits on the 17 V pad");
    assert(p != -pad_17v, "a pin would reach the 17 V pad with the pin block turned by 180 degrees");
}
assert(boss_h > 0.3, "boss too low: check window_depth");
assert(step_depth > 0.5, "nose too low to locate: check step_depth");
assert(step_w < window_w - 2 * window_fit && step_h < window_h - 2 * window_fit, "inner pocket larger than the outer recess");
assert(pin_compress <= 0.75 * pin_stroke, "pin compression too close to the full stroke");
assert(pitch_x - pin_relief_d >= 0.5, "pin holes merge: reduce pin_relief_d");
assert(pitch_x >= slot_w + tower_wall && pitch_y >= slot_w + tower_wall, "tower walls between the pins too thin");
assert(jig_block_z1 - block_z0 >= 1.4, "pin block too thin: raise jig_block_z1");
assert(jig_top_z - jig_block_z1 >= 3, "no room to solder wires to the pins between block and cap");
assert(jig_top_z - web_t >= cable_d + 0.3, "the cable does not fit between base and cap: reduce cable_d");
assert(block_shoulder < web_t - 0.6, "seat cone as deep as the base plate");
assert(screw_wall - (csk ? csk_depth : 0) >= 3, "less than 3 mm of plastic under the M4 head: longer screw or less m4_engage");
assert(screw_wall + (csk ? 0 : m4_washer_t + 4) <= jig_bot_z - 0.3, "the M4 head would stand proud of the cap, which is the foot of the jig");
assert(clamp_post_z + clamp_bar_t + screw_head_h <= jig_bot_z - 0.3, "the cable clamp would stand proud of the cap, which is the foot of the jig: raise cap_floor");

echo(str("cap ", jig_top_z, " to ", jig_bot_z, " mm above the plate surface; free pin tip ", tip_depth,
         " mm below it; hub ", screw_wall, " mm high"));

// ---------------------------------------------------------------- helpers

module rrect(w, h, r) {
    offset(r) square([w - 2 * r, h - 2 * r], center = true);
}

// Outline of the boss in the outer recess, grown by e. It is also the neck
// through the base and, grown, the seat.
module boss_rect(e) {
    translate([window_x, window_y])
        rrect(window_w - 2 * window_fit + 2 * e, window_h - 2 * window_fit + 2 * e,
              max(window_r - window_fit + e, 0.5));
}

// outline of the nose in the inner pocket, grown by e
module nose_rect(e) {
    translate([window_x, window_y])
        rrect(step_w - 2 * fit + 2 * e, step_h - 2 * fit + 2 * e, max(nose_r + e, 0.5));
}

// boss outline from z0 (grown by e0) to z1 (grown by e1)
module boss_loft(z0, z1, e0, e1) {
    hull() {
        translate([0, 0, z0]) linear_extrude(eps) boss_rect(e0);
        translate([0, 0, z1 - eps]) linear_extrude(eps) boss_rect(e1);
    }
}

// guide hole in the nose, a wider bore behind it up to z_end
module pin_holes(z_end) {
    g0 = -nose_z + pin_guide_len;             // end of the guide hole
    cone = (pin_relief_d - pin_guide_d) / 2;
    for (p = pins)
        translate(concat(cell(p), 0)) {
            translate([0, 0, -nose_z - 1]) cylinder(d = pin_guide_d, h = pin_guide_len + 1 + eps, $fn = 32);
            translate([0, 0, g0]) cylinder(d1 = pin_guide_d, d2 = pin_relief_d, h = cone + eps, $fn = 32);
            translate([0, 0, g0 + cone]) cylinder(d = pin_relief_d, h = z_end - g0 - cone + 1, $fn = 32);
        }
}

// flat part of thickness t with the elephant-foot chamfer on the bed face
module bed_plate(t) {
    linear_extrude(bed_chamfer + eps) offset(delta = -bed_chamfer) children();
    translate([0, 0, bed_chamfer]) linear_extrude(t - bed_chamfer) children();
}

// ---------------------------------------------------------------- pin block

// Nose, boss and neck through the base, 45 degree seat, flange behind it,
// and the pin tower on the flange. The nose in the inner pocket locates the
// pins; the boss in the outer recess is loose and grows out of the nose at
// 45 degrees, so the block prints nose down.
module block() {
    c = min(boss_chamfer, step_depth / 2);
    lift = min(boss_h, max(window_w - 2 * window_fit - step_w + 2 * fit,
                           window_h - 2 * window_fit - step_h + 2 * fit) / 2);
    seat_z = block_z0 - block_shoulder;
    difference() {
        union() {
            hull() {
                translate([0, 0, -nose_z]) linear_extrude(eps) nose_rect(-c);
                translate([0, 0, -nose_z + c]) linear_extrude(nose_z - boss_h - c + eps) nose_rect(0);
            }
            hull() {
                translate([0, 0, -boss_h]) linear_extrude(eps) nose_rect(0);
                translate([0, 0, -boss_h + lift - eps]) linear_extrude(eps) boss_rect(0);
            }
            boss_loft(-boss_h + lift - eps, seat_z, 0, 0);
            boss_loft(seat_z - eps, block_z0, 0, block_shoulder);
            boss_loft(block_z0 - eps, jig_block_z1, block_shoulder, block_shoulder);
            tower();
        }
        pin_holes(jig_block_z1 - 1);          // the wide bore ends in the flange, the tower guides again
    }
}

// Tower on the back of the pin block, up to the cap. Its lower part is a
// closed guide around each pin, so that the pin stands upright; the top
// slot_depth is a slot per pin, in which the solder joint and the wire lie
// and leave sideways. The walls between the slots keep the three joints
// apart. Solid over the 17 V cell.
module tower() {
    slot_z = tower_top_z - slot_depth;        // floor of the slots
    lead = (slot_w - pin_guide_d) / 2;
    assert(slot_z - jig_block_z1 >= 1.2, "tower guide too short: reduce slot_depth");
    difference() {
        translate([tower_lo[0], tower_lo[1], jig_block_z1 - eps])
            cube([tower_hi[0] - tower_lo[0], tower_hi[1] - tower_lo[1], tower_top_z - jig_block_z1 + eps]);
        for (i = [0 : len(pins) - 1]) {
            c = cell(pins[i]);
            hull() for (k = [0, 10])
                translate([c[0] + k * pin_exits[i][0], c[1] + k * pin_exits[i][1], slot_z])
                    cylinder(d = slot_w, h = tower_top_z, $fn = 24);
            // guide below the slot, with a lead-in for the pin tip
            translate([c[0], c[1], jig_block_z1 - 1]) cylinder(d = pin_guide_d, h = slot_z - jig_block_z1 + 1 + eps, $fn = 32);
            translate([c[0], c[1], slot_z - lead]) cylinder(d1 = pin_guide_d, d2 = slot_w, h = lead + eps, $fn = 32);
        }
    }
}

// ---------------------------------------------------------------------- cap

// outline of the cap: around its two posts and over the tower
module cap_2d() {
    hull() {
        for (p = jig_posts) translate([window_x + p[0], window_y + p[1]]) circle(d = post_d + 1, $fn = 48);
        translate([(tower_lo[0] + tower_hi[0]) / 2, (tower_lo[1] + tower_hi[1]) / 2])
            rrect(tower_hi[0] - tower_lo[0] + 2, tower_hi[1] - tower_lo[1] + 2, 2);
    }
}

// A flat plate screwed to the two posts of the base. The pin ends stand
// against its face, so it takes the spring force and sets the compression;
// it also closes the tower and keeps the pin block in its seat. Its screw
// heads are sunk, so its table side is flat and the Echo can stand on it.
// Printed Echo side down.
module cap() {
    sink = screw_head_h + 0.3;
    difference() {
        translate([0, 0, jig_top_z]) linear_extrude(jig_bot_z - jig_top_z) cap_2d();
        for (p = jig_posts)
            translate([window_x + p[0], window_y + p[1], 0]) {
                cylinder(d = m2_clear_d, h = jig_bot_z + 1, $fn = 24);
                translate([0, 0, jig_bot_z - sink]) cylinder(d = 4.6, h = sink + 1, $fn = 32);
            }
    }
}

// TPU shoe for the cap: a sole under it with a lip that stretches over its edge
module tpu_shoe() {
    difference() {
        translate([0, 0, jig_bot_z - tpu_lip]) linear_extrude(tpu_lip + tpu_t) offset(r = tpu_wall - tpu_fit) cap_2d();
        translate([0, 0, jig_bot_z - tpu_lip - 1]) linear_extrude(tpu_lip + 1) offset(delta = -tpu_fit) cap_2d();
    }
}

// TPU sock for a foot of the base, drawn at the origin: a cup with a sole
module tpu_sock() {
    difference() {
        translate([0, 0, jig_bot_z - tpu_cup]) cylinder(d = feet_d - 2 * tpu_fit + 2 * tpu_wall, h = tpu_cup + tpu_t, $fn = 64);
        translate([0, 0, jig_bot_z - tpu_cup - 1]) cylinder(d = feet_d - 2 * tpu_fit, h = tpu_cup + 1, $fn = 64);
    }
}

// --------------------------------------------------------------------- base

// Seat for the pin block in the base plate. Above the plate the cut closes
// again at 45 degrees, so that a post standing partly over the seat grows
// over it without a flat overhang.
module block_seat() {
    e1 = neck_fit + block_shoulder;
    roof = post_d / 2;
    boss_loft(-1, web_t - block_shoulder, neck_fit, neck_fit);
    boss_loft(web_t - block_shoulder - eps, web_t + eps, neck_fit, e1);
    boss_loft(web_t, web_t + roof, e1, e1 - roof);
}

module m4_hole() {
    translate([0, 0, -1]) cylinder(d = m4_clear_d, h = screw_wall + 2);
    if (csk) {
        translate([0, 0, screw_wall - csk_depth]) cylinder(d1 = m4_clear_d, d2 = csk_d, h = csk_depth + eps);
        translate([0, 0, screw_wall]) cylinder(d = csk_d, h = 1);
    }
    if (insert_relief_h > 0)
        translate([0, 0, -eps]) cylinder(d = insert_relief_d, h = insert_relief_h + eps);
}

// The base, also the test-fit coupon: a plate with the screw seat, the seat
// for the pin block, two posts for the cap, and the cable clamp (cradle and
// two posts). Its table side names the pins, read in the device view.
// Printed with the face that lies on the Echo on the bed.
// feet = true: two arms from the hub, each ending in a foot as high as the
// cap, so that the Echo stands upright on the cap and the two feet.
module base(feet = false) {
    pilot = jig_top_z - 1.2;
    module label(u, v, s, ha)
        translate([window_x + u, window_y + v, web_t - label_depth]) linear_extrude(label_depth + 0.5)
            offset(delta = 0.08)
                text(s, size = label_size, font = "Liberation Sans:style=Bold", halign = ha, valign = "center");
    module plate_2d() {
        hull() {
            circle(d = hub_d);
            translate([window_x, window_y]) rrect(2 * jig_plate_u, window_h + 12, 4);
            for (p = jig_posts) translate([window_x + p[0], window_y + p[1]]) circle(d = post_d + 2);
        }
        // tab for the cable clamp
        hull() {
            for (s = [-1, 1]) translate([window_x + cable_u + s * clamp_u, window_y - clamp_v]) circle(d = post_d + 2);
            translate([window_x + cable_u, window_y - 8]) square([2 * clamp_u + post_d + 2, 2], center = true);
        }
        if (feet) for (a = feet_angles) hull() {
            circle(d = feet_arm_w);
            rotate(window_a + a) translate([feet_r, 0]) circle(d = feet_d);
        }
    }
    difference() {
        union() {
            bed_plate(web_t) plate_2d();
            cylinder(d = hub_d, h = screw_wall);
            for (p = jig_posts)
                translate([window_x + p[0], window_y + p[1], web_t - eps])
                    cylinder(d = post_d, h = jig_top_z - web_t + eps, $fn = 48);
            // cable clamp: two posts for the bar and a cradle between them
            for (s = [-1, 1])
                translate([window_x + cable_u + s * clamp_u, window_y - clamp_v, web_t - eps])
                    cylinder(d = post_d, h = clamp_post_z - web_t + eps, $fn = 48);
            // narrower than the posts, so that it ends inside them
            translate([window_x + cable_u - clamp_u, window_y - clamp_v - (post_d - 1) / 2, web_t - eps])
                cube([2 * clamp_u, post_d - 1, 2 + eps]);
            if (feet) for (a = feet_angles)
                rotate(window_a + a) translate([feet_r, 0, web_t - eps]) cylinder(d = feet_d, h = jig_bot_z - web_t + eps, $fn = 64);
        }
        m4_hole();
        block_seat();
        for (p = jig_posts)
            translate([window_x + p[0], window_y + p[1], jig_top_z - pilot])
                cylinder(d = screw_pilot_d, h = pilot + eps, $fn = 24);
        // cable clamp: groove of the cradle, pilots in the posts
        translate([window_x + cable_u, window_y - clamp_v + clamp_w, cable_z]) rotate([90, 0, 0])
            cylinder(d = cable_d, h = 2 * clamp_w, $fn = 48);
        for (s = [-1, 1])
            translate([window_x + cable_u + s * clamp_u, window_y - clamp_v, 1])
                cylinder(d = screw_pilot_d, h = clamp_post_z, $fn = 24);
        // D+ starts clear of the cap post beside it (jig_posts[0]), D- follows
        label(jig_posts[0][0] + post_d / 2 + 0.2, 8.9, "D+", "left");
        label(jig_posts[0][0] + post_d / 2 + 0.45 + 2.03 * label_size, 8.9, "D-", "left");
        label(pin_gnd[0] * pitch_x - 0.6, -8.9, "GND", "center");   // below the GND hole, clear of the cap post
    }
}

// clamp bar: screwed onto the two clamp posts of the base, across the cable
module clamp() {
    difference() {
        translate([window_x + cable_u, window_y - clamp_v, clamp_post_z]) linear_extrude(clamp_bar_t)
            rrect(2 * clamp_u + post_d + 1, clamp_w, 2);
        for (s = [-1, 1])
            translate([window_x + cable_u + s * clamp_u, window_y - clamp_v, clamp_post_z - 1])
                cylinder(d = m2_clear_d, h = clamp_bar_t + 2, $fn = 24);
    }
}

// --------------------------------------------------------- section plane

// section_pins: plane through the D+ and D- pins, parallel to x.
cut = part == "section_pins";
cut_y = cell(pin_dp)[1];

// translate and remember the offset, so col() can cut in the device frame
$org = [0, 0, 0];
module at(v) {
    $org = v;
    translate(v) children();
}

// colour a display object; in the section view keep only the half behind the
// plane, so that each cut face keeps the colour of its part
module col(c) {
    color(c)
        if (cut) intersection() {
            union() { children(); }
            translate(-$org) translate([-200, cut_y - 400, -200]) cube(400);
        }
        else children();
}

// ---------------------------------------------------- display-only objects

module window_2d() {
    rrect(window_w, window_h, window_r);
}

module echo_ghost() {
    h = 30;
    col([0.27, 0.28, 0.31]) difference() {
        rotate_extrude() polygon([[0, 0], [plate_d / 2 + rim_w, 0], [plate_d / 2 + rim_w + 1.5, -1],
                                  [46, -4], [49.5, -12], [49.5, -h], [0, -h]]);
        translate([window_x, window_y, -window_depth]) linear_extrude(window_depth + 1) window_2d();
        translate([window_x, window_y, -window_depth - step_depth])
            linear_extrude(step_depth + eps) rrect(step_w, step_h, 1);
        for (c = all_cells)
            translate(concat(cell(c), -pad_depth)) cylinder(d = hole_d, h = pad_depth, $fn = 32);
        translate([0, 0, -9]) cylinder(d = 4, h = 10);
        // seam between the plate and the rim
        translate([0, 0, -0.15]) difference() {
            cylinder(d = plate_d + 0.3, h = 1);
            translate([0, 0, -1]) cylinder(d = plate_d - 0.3, h = 3);
        }
    }
    col("silver") difference() {
        translate([0, 0, -9]) cylinder(d = 7.5, h = 9 - eps);
        translate([0, 0, -9.5]) cylinder(d = 3.4, h = 10);
    }
    for (c = all_cells)
        col(c == pad_17v ? "red" : "gold")
            translate(concat(cell(c), -pad_depth - 0.1)) cylinder(d = pad_d, h = 0.1, $fn = 32);
    col("darkgreen") translate([window_x, window_y, -pad_depth - 1.1])
        linear_extrude(1) square([step_w + 4, step_h + 4], center = true);
}

// a pin with its closed end against the cap; compressed: tip on the pad
module pin_ghost(compressed) {
    plunger = 3.2;                            // plunger outside the barrel when free
    tail_z = pin_len - tip_depth;
    barrel_z = plunger - tip_depth;
    tip_z = compressed ? -pad_depth : -tip_depth;
    col("darkgoldenrod") translate([0, 0, barrel_z])
        cylinder(d = pin_d, h = tail_z - barrel_z, $fn = 24);
    col("gold") {
        translate([0, 0, tip_z + 0.6]) cylinder(d = 0.74, h = barrel_z - tip_z - 0.6 + eps, $fn = 24);
        translate([0, 0, tip_z]) cylinder(d1 = 0.05, d2 = 0.74, h = 0.6, $fn = 24);
    }
}

module m4_ghost() {
    if (csk) {
        top = screw_wall - 0.3;
        col("dimgray") {
            translate([0, 0, top - m4_head_d / 2 + 2]) difference() {
                cylinder(d1 = 4, d2 = m4_head_d, h = m4_head_d / 2 - 2, $fn = 32);
                translate([0, 0, 0.8]) cylinder(d = 2.9, h = 3, $fn = 6);
            }
            translate([0, 0, top - m4_len]) cylinder(d = 3.9, h = m4_len - 1.9, $fn = 32);
        }
    }
    else {
        hz = screw_wall + m4_washer_t;
        col("dimgray") {
            translate([0, 0, hz]) difference() {
                cylinder(d = 7, h = 4, $fn = 32);
                translate([0, 0, 1.5]) cylinder(d = 3.5, h = 3, $fn = 6);
            }
            translate([0, 0, hz - m4_len]) cylinder(d = 3.9, h = m4_len, $fn = 32);
        }
        col("silver") translate([0, 0, screw_wall]) difference() {
            cylinder(d = 9, h = m4_washer_t, $fn = 32);
            translate([0, 0, -1]) cylinder(d = 4.3, h = 3, $fn = 32);
        }
    }
}

// soldered wires and cable
module wires() {
    colours = ["limegreen", "white", [0.1, 0.1, 0.1]];
    side = [-1.1, 1.1, 0];
    cab = [window_x + cable_u, window_y - clamp_v + clamp_w / 2 + 2, cable_z];   // the jacket ends just before the clamp
    z = jig_top_z - 2;
    module wire(pts)
        for (i = [0 : len(pts) - 2]) hull() for (p = [pts[i], pts[i + 1]]) translate(p) sphere(d = 1, $fn = 12);
    for (i = [0 : len(pins) - 1]) {
        c = cell(pins[i]);
        e = pin_exits[i];
        out = [c[0] + 3.6 * e[0], c[1] + 3.6 * e[1], z];
        col(colours[i]) wire([[c[0] + 0.7 * e[0], c[1] + 0.7 * e[1], z], out,
                              [out[0], min(out[1], window_y - 4.6), z], cab + [side[i], 0, 0]]);
    }
    col([0.12, 0.12, 0.12]) translate(cab) rotate([90, 0, 0]) cylinder(d = cable_d - 0.5, h = 26, $fn = 32);
}

// M2 screws of the cap and of the clamp
module m2_ghosts() {
    col("dimgray") {
        for (p = jig_posts) at([window_x + p[0], window_y + p[1], 0]) {
            translate([0, 0, jig_bot_z - screw_head_h - 0.3]) cylinder(d = 3.8, h = screw_head_h, $fn = 24);
            translate([0, 0, jig_bot_z - screw_head_h - 0.3 - 6]) cylinder(d = 2, h = 6, $fn = 24);
        }
        for (s = [-1, 1]) at([window_x + cable_u + s * clamp_u, window_y - clamp_v, 0]) {
            translate([0, 0, clamp_post_z + clamp_bar_t]) cylinder(d = 3.8, h = screw_head_h, $fn = 24);
            translate([0, 0, clamp_post_z + clamp_bar_t - 6]) cylinder(d = 2, h = 6, $fn = 24);
        }
    }
}

module assembly() {
    col([0.93, 0.55, 0.14]) base(on_feet);
    col([0.35, 0.7, 0.9]) block();
    col([0.55, 0.8, 0.45]) cap();
    col([0.55, 0.8, 0.45]) clamp();
    wires();
    m2_ghosts();
    if (on_feet) col([0.15, 0.15, 0.17]) {
        tpu_shoe();
        for (a = feet_angles) rotate(window_a + a) translate([feet_r, 0, 0]) tpu_sock();
    }
    for (p = pins) at(concat(cell(p), 0)) pin_ghost(show_echo);
    m4_ghost();
    if (show_echo) echo_ghost();
}

// Exploded view, table side up, as the jig is put together: the Echo lies
// upside down and everything is added from above. Display only.
exploded_vpr = [62, 0, 50];                   // camera rotation the labels face (see build.py)

module exploded() {
    right = [cos(exploded_vpr[2]), sin(exploded_vpr[2]), 0];

    module label(z, s)
        color("black") translate(64 * right + [0, 0, z]) rotate(exploded_vpr)
            linear_extrude(0.2) text(s, size = 4.6);

    translate([0, 0, -22]) echo_ghost();
    label(-28, "Echo upside down, unplugged");

    color([0.93, 0.55, 0.14]) base();
    label(2, "1  base on the Echo");

    translate([0, 0, 24]) color([0.35, 0.7, 0.9]) block();
    label(18, "2  pin block, nose first, through the base");

    translate([0, 0, 24]) m4_ghost();
    label(27, "3  M4 screw into the centre insert");

    translate([0, 0, 46]) {
        for (p = pins) translate(concat(cell(p), 0)) pin_ghost(false);
        wires();
    }
    label(47, "4  pins with their wires soldered on, tip first;");
    label(39, "    each wire lies in the slot next to its hole");

    translate([0, 0, 68]) color([0.55, 0.8, 0.45]) {
        cap();
        clamp();
    }
    label(74, "5  cap, flat on the two posts;");
    label(66, "    clamp bar across the cable");

    translate([0, 0, 86]) m2_ghosts();
    label(97, "6  four M2 screws: two in the cap, two in the clamp");
}

// --------------------------------------------------------------- top level

module oriented() {
    if (flip) rotate([180, 0, 0]) children();
    else children();
}

// Printable parts are exported as they lie on the bed: the plate surface for
// the bases, the nose face for the pin block, the Echo side for the cap, the
// sole for the TPU parts (turned over by a rotation, never mirrored).
if (part == "base") base();
else if (part == "base_feet") base(true);
else if (part == "block") translate([-window_x, -window_y, nose_z]) block();
else if (part == "cap") translate([-window_x, -window_y, -jig_top_z]) cap();
else if (part == "clamp") translate([-window_x - cable_u, -window_y + clamp_v, -clamp_post_z]) clamp();
else if (part == "tpu_shoe") translate([0, 0, jig_bot_z + tpu_t]) rotate([180, 0, 0]) translate([-window_x, -window_y, 0]) tpu_shoe();
else if (part == "tpu_sock") translate([0, 0, jig_bot_z + tpu_t]) rotate([180, 0, 0]) tpu_sock();
else if (part == "exploded") exploded();
else oriented() assembly();
