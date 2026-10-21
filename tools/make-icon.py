#!/usr/bin/env python3
"""Draws a placeholder app icon.

    python3 tools/make-icon.py apps/calculator/icon.png

The result is a 64x64 RGBA PNG: a Tokyo Night card with a display and a keypad.
It is a placeholder for an app-specific drawing, not part of the SDK.
"""

from __future__ import annotations

import struct
import sys
import zlib

SIZE = 64
BACKGROUND = (0x1A, 0x1B, 0x26, 0xFF)
DISPLAY = (0x7A, 0xA2, 0xF7, 0xFF)
KEY = (0xC0, 0xCA, 0xF5, 0xFF)
ACCENT = (0xBB, 0x9A, 0xF7, 0xFF)


def canvas() -> list[list[tuple[int, int, int, int]]]:
    return [[(0, 0, 0, 0) for _ in range(SIZE)] for _ in range(SIZE)]


def rounded_background(pixels) -> None:
    radius = 10
    for y in range(SIZE):
        for x in range(SIZE):
            inside = True
            cx = x + 0.5
            cy = y + 0.5
            if cx < radius and cy < radius:
                inside = (cx - radius) ** 2 + (cy - radius) ** 2 <= radius**2
            elif cx > SIZE - radius and cy < radius:
                inside = (cx - (SIZE - radius)) ** 2 + (cy - radius) ** 2 <= radius**2
            elif cx < radius and cy > SIZE - radius:
                inside = (cx - radius) ** 2 + (cy - (SIZE - radius)) ** 2 <= radius**2
            elif cx > SIZE - radius and cy > SIZE - radius:
                inside = (
                    (cx - (SIZE - radius)) ** 2 + (cy - (SIZE - radius)) ** 2
                    <= radius**2
                )
            if inside:
                pixels[y][x] = BACKGROUND


def rect(pixels, x0: int, y0: int, x1: int, y1: int, colour) -> None:
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < SIZE and 0 <= y < SIZE:
                pixels[y][x] = colour


def chunk(kind: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)


def write_png(path: str, pixels) -> None:
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for pixel in row:
            raw.extend(pixel)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as handle:
        handle.write(png)


def main(argv: list[str]) -> int:
    path = argv[1] if len(argv) > 1 else "icon.png"
    pixels = canvas()
    rounded_background(pixels)

    rect(pixels, 10, 9, 54, 24, DISPLAY)

    for row in range(4):
        for column in range(3):
            colour = ACCENT if row == 3 and column == 2 else KEY
            x = 10 + column * 16
            y = 28 + row * 9
            rect(pixels, x, y, x + 12, y + 6, colour)

    write_png(path, pixels)
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
