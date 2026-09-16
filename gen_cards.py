#!/usr/bin/env python3
"""gen_cards.py - render the four FLY title cards as 640x200 2bpp SHR bitmaps.

Spec section 5 copy, verbatim. Helvetica Neue, anti-aliased to 4 grey
levels (all a 640-mode palette group can hold), one palette per scanline:
palette 1 = headline ramp ($CFF), palette 2 = body ramp ($6EE). The GS
blits 32000 bytes of pixels + 200 SCBs per card and fades the two
palettes, so the text is as sharp as the IIgs can draw.

Output: FLYCARDS.BIN = 3 x (32000 px + 200 scb) = 96600 bytes,
        cards_preview.png (what the GS shows, upscaled, for a quick look)
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT = HERE / "FLYCARDS.BIN"
PREVIEW = HERE / "cards_preview.png"

TTC = "/System/Library/Fonts/HelveticaNeue.ttc"
FACE_HEAD = 10   # Medium
FACE_BODY = 0    # Regular

# Physical canvas is 4:3; the GS shows it as 640x200 (each 640 pixel is
# tall and narrow). Draw in physical space, resample to 640x200 at the end.
PW, PH = 1280, 960
YS = PH / 200.0   # physical px per scanline

# Headlines: Helvetica Neue Medium, anti-aliased (big enough to take it).
# Body: Geneva 12 rendered hinted and bilevel at its real pixel size, each
# column doubled, same as the viewer chrome. AA on 11-row text read as blur.
HEAD_ROWS = 22
HEAD_LEAD = 30
BODY_PX = 12          # Geneva pixel size = rows on screen
BODY_LEAD = 15
BLANK = 10
MAXW = 1200           # physical, leaves a margin on both sides
BODY_FONT = "/System/Library/Fonts/Geneva.ttf"

# ("text", is_headline). "" = blank line. Three cards.
CARDS = [
    [
        ("September 3, 2026", True),
        ("", False),
        ("Scientists published the first complete map", False),
        ("of a fruit fly's entire central nervous system:", False),
        ("brain and nerve cord, every neuron, every connection.", False),
        ("", False),
        ("166,700 neurons", True),
        ("125,000,000 synaptic connections", True),
    ],
    [
        ("September 15, 1986", True),
        ("", False),
        ("Apple announced the Apple IIgs.", False),
        ("16-bit 65C816.  2.8 MHz.  4,096 colors.", False),
    ],
    [
        ("Forty years later, this IIgs will download", False),
        ("real neurons from that map and process them.", False),
        ("", False),
        ("Live. On the 65C816.", True),
    ],
]


def font(face, rows):
    return ImageFont.truetype(TTC, int(round(rows * YS)), index=face)


def fit_rows(lines, face, rows):
    """Shrink a size until every line in the group fits MAXW."""
    while rows > 6:
        f = font(face, rows)
        if all(f.getlength(t) <= MAXW for t in lines):
            return rows
        rows -= 1
    return rows


def body_bitmap(text, px):
    """Hinted bilevel Geneva at px, ink-cropped, columns doubled -> 0/1 array."""
    f = ImageFont.truetype(BODY_FONT, px)
    # hinted advances run wider than getlength; give the canvas slack
    w = int(round(f.getlength(text))) * 2 + 16
    im = Image.new("1", (w, px + 6), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    d.text((1, 0), text, font=f, fill=1)
    a = np.asarray(im).astype(np.uint8)
    rows = np.nonzero(a.any(axis=1))[0]
    cols = np.nonzero(a.any(axis=0))[0]
    a = a[rows.min():rows.max() + 1, cols.min():cols.max() + 1]
    return np.repeat(a, 2, axis=1), int(rows.min())


def body_px_for(lines):
    """Largest Geneva size (<= BODY_PX) whose doubled ink width fits every line."""
    px = BODY_PX
    while px > 8:
        if all(body_bitmap(t, px)[0].shape[1] <= 624 for t in lines):
            return px
        px -= 1
    return px


def render_card(lines):
    heads = [t for t, h in lines if h and t]
    hrows = fit_rows(heads, FACE_HEAD, HEAD_ROWS) if heads else HEAD_ROWS
    hlead = int(round(HEAD_LEAD * hrows / HEAD_ROWS))
    fh = font(FACE_HEAD, hrows)
    bpx = body_px_for([t for t, h in lines if not h and t])

    total = 0
    for t, h in lines:
        total += BLANK if not t else (hlead if h else BODY_LEAD)
    y_rows = (200 - total) / 2.0

    img = Image.new("L", (PW, PH), 0)
    d = ImageDraw.Draw(img)
    role = np.zeros(200, dtype=np.uint8)   # 1 head, 2 body, 0 none
    bodies = []                            # (y_row, bitmap) placed after resample
    for t, h in lines:
        if not t:
            y_rows += BLANK
            continue
        if h:
            w = fh.getlength(t)
            x = (PW - w) / 2.0
            y = y_rows * YS
            d.text((x, y), t, font=fh, fill=255)
            bbox = d.textbbox((x, y), t, font=fh)
            r0 = max(0, int(bbox[1] / YS) - 1)
            r1 = min(199, int(bbox[3] / YS) + 1)
            role[r0:r1 + 1] = 1
            y_rows += hlead
        else:
            bm, top = body_bitmap(t, bpx)
            y0 = int(round(y_rows)) + top
            bodies.append((y0, bm))
            role[max(0, y0 - 1):min(199, y0 + bm.shape[0]) + 1] = 2
            y_rows += BODY_LEAD

    small = img.resize((640, 200), Image.LANCZOS)
    g = np.asarray(small, dtype=np.float32) / 255.0
    lvl = np.digitize(g, [0.18, 0.45, 0.75]).astype(np.uint8)   # 0..3
    for y0, bm in bodies:
        h, w = bm.shape
        x0 = (640 - w) // 2
        x0 -= x0 & 1
        lvl[y0:y0 + h, x0:x0 + w] = np.maximum(lvl[y0:y0 + h, x0:x0 + w], bm * 3)
    return lvl, role


def pack_640(lvl):
    """4 pixels per byte, leftmost pixel in bits 7-6."""
    out = bytearray()
    for y in range(200):
        row = lvl[y]
        for x in range(0, 640, 4):
            b = (int(row[x]) << 6) | (int(row[x + 1]) << 4) | (int(row[x + 2]) << 2) | int(row[x + 3])
            out.append(b)
    assert len(out) == 32000
    return bytes(out)


def preview(all_lvl, all_role):
    head = np.array([0xCC, 0xFF, 0xFF]) / 255.0
    body = np.array([0x66, 0xEE, 0xEE]) / 255.0
    tiles = []
    for lvl, role in zip(all_lvl, all_role):
        rgb = np.zeros((200, 640, 3), dtype=np.float32)
        for y in range(200):
            c = head if role[y] == 1 else body
            rgb[y] = (lvl[y][:, None] / 3.0) * c[None, :]
        im = Image.fromarray((rgb * 255).astype(np.uint8))
        im = im.resize((1280, 960), Image.NEAREST)
        tiles.append(im)
    sheet = Image.new("RGB", (1280, 960 * len(tiles)), 0)
    for i, t in enumerate(tiles):
        sheet.paste(t, (0, i * 960))
    sheet.save(PREVIEW)


def main():
    blob = bytearray()
    all_lvl, all_role = [], []
    for i, lines in enumerate(CARDS):
        lvl, role = render_card(lines)
        scb = bytes(0x80 | (1 if r != 2 else 2) for r in role)   # 640 mode + palette
        blob += pack_640(lvl) + scb
        all_lvl.append(lvl)
        all_role.append(role)
        print(f"card {i + 1}: {len(lines)} lines, text rows {int((role > 0).sum())}")
    OUT.write_bytes(blob)
    preview(all_lvl, all_role)
    print(f"wrote {OUT} ({len(blob)} bytes) and {PREVIEW}")


if __name__ == "__main__":
    main()
