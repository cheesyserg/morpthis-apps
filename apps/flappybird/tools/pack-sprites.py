#!/usr/bin/env python3
"""Pack the Flappy Bird artwork into the app's sprite format.

The device has no PNG decoder in API 1, and a decoded image is what keeps the
drawings exact: `MSPR` carries RGB565 plus a one-bit transparency mask, so a
transparent pixel stays transparent instead of picking up the white (or black)
that sat under it in the PNG.

Usage:

    pack-sprites.py <art-dir> [<out-dir>]

<art-dir> is the original artwork folder (the 1x PNGs from the player's
`sonix-packer/assets/R1/usr/resource/sonix/gui/flappybird`). <out-dir> defaults
to `assets/sprites` beside this tool.

The pixel sizes are the game's own (144x256 background, 168x56 ground, 26x160
pipes, 17x12 birds); the renderer scales them on the panel.
"""

import os
import struct
import sys
import zlib

# name: (width, height); checked, because the renderer relies on them.
EXPECTED = {
    "BG-day": (144, 256),
    "BG-night": (144, 256),
    "ground": (168, 56),
    "pipe-up": (26, 160),
    "pipe-down": (26, 160),
    "bird": (17, 12),
}

BIRDS = ["yellow", "red", "blue", "green", "peach", "purple", "cow", "rare"]
MEDALS = ["bronze", "silver", "gold"]

PLAIN = [
    "BG-day",
    "BG-night",
    "ground",
    "pipe-up",
    "pipe-down",
    "logo",
    "get-ready",
    "how-to",
    "game-over",
    "score-board",
    "new-record-label",
    "play-button",
    "menu",
    "exit-button",
    "sound-on",
    "sound-off",
]


def names():
    for name in PLAIN:
        yield name, name
    for colour in BIRDS:
        for i in range(1, 4):
            yield "bird-%s-%02d" % (colour, i), "bird"
    for i in range(10):
        yield "score-game-%d" % i, None
        yield "score-%d" % i, None
    for medal in MEDALS:
        yield "medal-%s" % medal, None


def read_png(path):
    """Decode an 8-bit RGBA PNG to a list of rows of (r, g, b, a)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)

    pos = 8
    idat = b""
    width = height = depth = colour = 0
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 6 or interlace != 0:
                raise ValueError("%s: expected 8-bit non-interlaced RGBA" % path)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    rows = []
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        filt = raw[at]
        at += 1
        line = bytearray(raw[at:at + stride])
        at += stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if filt == 1:
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + predictor) & 0xFF
            elif filt != 0:
                raise ValueError("%s: filter %d" % (path, filt))
        rows.append(line)
        previous = line

    return width, height, rows


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def pack(name, width, height, rows, out_dir):
    pixels = bytearray()
    mask = bytearray()
    any_clear = False
    for y in range(height):
        row = rows[y]
        for x in range(width):
            r, g, b, a = row[x * 4:x * 4 + 4]
            # The source is one-bit alpha; a half-covered edge would be a
            # fringe, so only pixels mostly covered are drawn.
            on = a >= 128
            pixels += struct.pack("<H", rgb565(r, g, b) if on else 0)
            mask.append(255 if on else 0)
            any_clear = any_clear or not on

    header = b"MSPR" + struct.pack("<HH", width, height) + bytes([1 if any_clear else 0, 0, 0, 0])
    path = os.path.join(out_dir, name + ".spr")
    with open(path, "wb") as f:
        f.write(header)
        f.write(pixels)
        if any_clear:
            f.write(mask)
    return path


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[0])
        print("usage: %s <art-dir> [<out-dir>]" % sys.argv[0], file=sys.stderr)
        return 2

    art_dir = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "assets", "sprites")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    for name, kind in names():
        path = os.path.join(art_dir, name + ".png")
        width, height, rows = read_png(path)
        want = EXPECTED.get(kind)
        if want and (width, height) != want:
            raise SystemExit("%s: %dx%d, expected %dx%d" % (path, width, height, want[0], want[1]))
        pack(name, width, height, rows, out_dir)
        print("%-22s %3dx%-3d" % (name, width, height))

    print("%d sprites in %s" % (len(list(names())), out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
