# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Export the printable parts (STL) and the preview renders (PNG).

Usage: python build.py [--openscad PATH] [--no-renders]

OpenSCAD is taken from --openscad, the OPENSCAD environment variable or the
PATH. The renders need a 2024 or later build (coloured --render output) and
Pillow for the captions; the STL export also works with 2021.01.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCAD = HERE / "pogo-jig.scad"
OUT = HERE / "stl"
RENDERS = HERE / "renders"

PARTS = {"base": "jig-base", "base_feet": "jig-base-feet", "block": "jig-pin-block", "cap": "jig-cap",
         "clamp": "jig-cable-clamp", "tpu_shoe": "tpu-cap-shoe", "tpu_sock": "tpu-foot-sock"}

# name: (part, flip, show_echo, camera "cx,cy,cz,rx,ry,rz,dist", size, caption[, on_feet])
# With flip the Echo side is up and device y points away from the viewer's
# "up" (the model is rotated by 180 degrees about x, not mirrored).
VIEWS = {
    "jig-in-use": ("assembly", False, True, "24,8,0,62,0,35,170", (1600, 1100),
                   "Flash jig on the Echo, which lies upside down: base (orange), cap and clamp bar (green), "
                   "cable in its clamp."),
    "jig-section-detail": ("section_pins", False, True, "25,0,2,90,0,180,58", (2000, 1100),
                           "Section through the D+ and D- pins, Echo below: nose in the inner pocket, "
                           "pins on the pads, wires soldered on in the slots of the tower, under the cap."),
    "jig-print-base": ("base", False, False, "17,6,3,55,0,25,130", (1400, 1000),
                       "Base as printed; it is also the test-fit coupon."),
    "jig-print-block": ("block", False, False, "0,0,6,60,0,30,62", (1400, 1000),
                        "Pin block as printed, nose face on the bed: tower with one slot per pin "
                        "(D+ and D- to the sides, GND to the front)."),
    "jig-print-cap": ("cap", False, False, "0,0,2,50,0,25,95", (1400, 1000),
                      "Cap as printed, Echo side on the bed: the screw heads sink into the table side."),
    "jig-print-clamp": ("clamp", False, False, "0,0,1,55,0,25,60", (1400, 900),
                        "Cable clamp bar as printed."),
    # the camera rotation must match exploded_vpr in pogo-jig.scad (the labels face it)
    "jig-exploded": ("exploded", False, True, "55,62,22,62,0,50,470", (2000, 1320),
                     "Assembly order. Table side up: the Echo lies upside down."),
    "jig-feet-in-use": ("assembly", True, True, "0,0,8,84,0,20,330", (1600, 1000),
                        "Flash jig on its feet: the Echo (dark, cut off) stands on the cap of the jig and on two feet.",
                        True),
    "jig-feet-in-use-back": ("assembly", True, True, "0,0,8,84,0,200,330", (1600, 1000),
                             "The same from the opposite side.", True),
    "jig-feet-underside": ("assembly", False, False, "0,0,6,50,0,20,260", (1600, 1100),
                           "Flash jig on its feet from the table side: the cap and the two feet, here in their "
                           "TPU shoe and socks (dark), touch the table.", True),
    "jig-feet-print": ("base_feet", False, False, "0,0,6,55,0,25,260", (1600, 1100),
                       "Base with feet as printed: flat face on the bed."),
}


def find_openscad(arg):
    for cand in (arg, os.environ.get("OPENSCAD"), shutil.which("openscad")):
        if cand and Path(cand).exists():
            return str(cand)
    sys.exit("OpenSCAD not found: pass --openscad or set OPENSCAD")


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    log = res.stdout + res.stderr
    if res.returncode != 0 or "WARNING" in log or "ERROR" in log:
        sys.exit("OpenSCAD failed:\n" + " ".join(cmd) + "\n" + log)
    return log


def caption(path, text):
    from PIL import Image, ImageDraw, ImageFont
    img = Image.open(path).convert("RGB")
    try:
        font = ImageFont.truetype("arial.ttf", 30)
    except OSError:
        font = ImageFont.load_default()
    lines, line = [], ""
    for word in text.split():
        if line and font.getlength(f"{line} {word}") > img.width - 32:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}".strip()
    lines.append(line)
    bar = Image.new("RGB", (img.width, 20 + 36 * len(lines)), "white")
    ImageDraw.Draw(bar).text((16, 10), "\n".join(lines), fill="black", font=font)
    out = Image.new("RGB", (img.width, img.height + bar.height), "white")
    out.paste(img, (0, 0))
    out.paste(bar, (0, img.height))
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--openscad")
    ap.add_argument("--no-renders", action="store_true")
    args = ap.parse_args()
    exe = find_openscad(args.openscad)
    version = subprocess.run([exe, "--version"], capture_output=True, text=True)
    modern = "2021" not in (version.stdout + version.stderr)
    backend = ["--backend=manifold"] if modern else []

    OUT.mkdir(exist_ok=True)
    for part, name in PARTS.items():
        log = run([exe, *backend, "-D", f'part="{part}"', "--export-format", "binstl",
                   "-o", str(OUT / f"{name}.stl"), str(SCAD)])
        note = next((l for l in log.splitlines() if l.startswith("ECHO")), "")
        print(f"{name}.stl", note)

    if args.no_renders:
        return
    if not modern:
        sys.exit("renders need OpenSCAD 2024 or later")
    RENDERS.mkdir(exist_ok=True)
    for name, (part, flip, echo, cam, size, text, *rest) in VIEWS.items():
        png = RENDERS / f"{name}.png"
        run([exe, *backend, "--render", "-D", f'part="{part}"',
             "-D", f"on_feet={'true' if rest and rest[0] else 'false'}",
             "-D", f"flip={'true' if flip else 'false'}",
             "-D", f"show_echo={'true' if echo else 'false'}",
             "--projection=o", f"--camera={cam}", f"--imgsize={size[0]},{size[1]}",
             "--colorscheme=Tomorrow", "-o", str(png), str(SCAD)])
        caption(png, text)
        print(png.name)


if __name__ == "__main__":
    main()
