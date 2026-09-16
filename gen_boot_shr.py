#!/usr/bin/env python3
"""gen_boot_shr.py - the boot-console backdrop: The Fly poster reduced to the
top 100 rows (head and upper glow, bottom edge faded to black) on a black
320x200, then through img2shr with palette 1 reserved for the 640 console.

Writes fly/FLYBOOT.shr (raw $C1/0000, 32768 bytes, unpacked).
"""
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SRC = ROOT / "Images" / "The Fly.jpg"
PNG = HERE / "flyboot_320.png"
OUT = HERE / "FLYBOOT.shr"
IMG2SHR = ROOT / "cogs" / "apps" / "imagemanager-mac" / "tools" / "img2shr.py"

BAND_Y = 96           # console starts here; poster gets rows 0..95
FADE_ROWS = 22        # bottom of the poster crop eases into the black

im = Image.open(SRC).convert("RGB")
W, H = im.size                                   # 1024 x 576
crop = im.crop((W * 3 // 16, 0, W * 13 // 16, H * 5 // 6))   # 640 x 480, drop black sides + tail
h = BAND_Y
w = int(round(crop.width * h / crop.height))     # keep the crop's aspect on the GS
small = crop.resize((w, h), Image.LANCZOS)

px = small.load()
for y in range(h - FADE_ROWS, h):
    k = (h - y) / float(FADE_ROWS)
    for x in range(w):
        r, g, b = px[x, y]
        px[x, y] = (int(r * k), int(g * k), int(b * k))

canvas = Image.new("RGB", (320, 200), (0, 0, 0))
canvas.paste(small, ((320 - w) // 2, 0))
canvas.save(PNG)

cmd = [sys.executable, str(IMG2SHR), str(PNG), str(OUT), "--reserve-pal", "1", "--unpacked32k"]
print(" ".join(cmd))
subprocess.check_call(cmd)
print("poster %dx%d at top, console band from row %d" % (w, h, BAND_Y))
