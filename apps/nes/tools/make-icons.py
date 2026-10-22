#!/usr/bin/env python3
"""Draws the app icon and the small cartridge used in the ROM list.

    python3 tools/make-icons.py

Both images are RGBA PNGs made without a graphics library, so regenerating them
needs only Python's standard library. The drawings are deliberately flat pixel
art: the player scales the icon down to its tile and the cartridge to a list
row, where anti-aliasing would only turn into grey fringes.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

APP = Path(__file__).resolve().parent.parent

TRANSPARENT = (0, 0, 0, 0)
BODY = (0x25, 0x29, 0x35, 0xFF)
BODY_EDGE = (0x3A, 0x40, 0x52, 0xFF)
BODY_LIGHT = (0x30, 0x35, 0x45, 0xFF)
LABEL = (0xEC, 0xE4, 0xD0, 0xFF)
LABEL_EDGE = (0xC9, 0xBE, 0xA4, 0xFF)
RED = (0xD2, 0x47, 0x42, 0xFF)
INK = (0x1C, 0x1E, 0x26, 0xFF)
GOLD = (0xD9, 0xB4, 0x5A, 0xFF)
GOLD_DARK = (0xAE, 0x8A, 0x35, 0xFF)

FONT = {
    "N": (0x11, 0x19, 0x15, 0x13, 0x11),
    "E": (0x1F, 0x10, 0x1E, 0x10, 0x1F),
    "S": (0x1F, 0x10, 0x0E, 0x01, 0x1F),
}


def canvas(size: int):
    return [[TRANSPARENT for _ in range(size)] for _ in range(size)]


def put(pixels, x: int, y: int, colour) -> None:
    if 0 <= y < len(pixels) and 0 <= x < len(pixels[y]):
        pixels[y][x] = colour


def rect(pixels, x0: int, y0: int, x1: int, y1: int, colour) -> None:
    for y in range(y0, y1):
        for x in range(x0, x1):
            put(pixels, x, y, colour)


def rounded(pixels, x0: int, y0: int, x1: int, y1: int, radius: int, colour,
            edge=None) -> None:
    for y in range(y0, y1):
        for x in range(x0, x1):
            near_x = min(x - x0, x1 - 1 - x)
            near_y = min(y - y0, y1 - 1 - y)
            if near_x < radius and near_y < radius:
                dx = radius - near_x - 0.5
                dy = radius - near_y - 0.5
                if dx * dx + dy * dy > radius * radius:
                    continue
            put(pixels, x, y, colour)
    if edge is None:
        return
    for x in range(x0 + radius, x1 - radius):
        put(pixels, x, y0, edge)
        put(pixels, x, y1 - 1, edge)
    for y in range(y0 + radius, y1 - radius):
        put(pixels, x0, y, edge)
        put(pixels, x1 - 1, y, edge)


def text(pixels, value: str, x: int, y: int, scale: int, colour) -> None:
    cursor = x
    for letter in value:
        rows = FONT[letter]
        for row, bits in enumerate(rows):
            for column in range(5):
                if bits & (1 << (4 - column)):
                    rect(pixels, cursor + column * scale, y + row * scale,
                         cursor + (column + 1) * scale, y + (row + 1) * scale, colour)
        cursor += 6 * scale


def cartridge(pixels, x0: int, y0: int, width: int, height: int) -> None:
    x1 = x0 + width
    y1 = y0 + height
    rounded(pixels, x0, y0, x1, y1, max(3, width // 10), BODY, BODY_EDGE)
    rect(pixels, x0 + 2, y0 + 2, x0 + 5, y1 - 2, BODY_LIGHT)

    label_x0 = x0 + width // 6
    label_x1 = x1 - width // 6
    label_y0 = y0 + height // 8
    label_y1 = y0 + height * 3 // 4
    rect(pixels, label_x0, label_y0, label_x1, label_y1, LABEL)
    rect(pixels, label_x0, label_y0, label_x1, label_y0 + 1, LABEL_EDGE)
    rect(pixels, label_x0, label_y1 - 1, label_x1, label_y1, LABEL_EDGE)
    rect(pixels, label_x0, label_y0, label_x0 + 1, label_y1, LABEL_EDGE)
    rect(pixels, label_x1 - 1, label_y0, label_x1, label_y1, LABEL_EDGE)

    stripe = max(2, height // 20)
    rect(pixels, label_x0 + 2, label_y0 + 2, label_x1 - 2, label_y0 + 2 + stripe, RED)

    # The teeth on the edge connector.
    tooth = max(2, width // 16)
    gap = max(2, width // 10)
    bottom = y1 - max(2, height // 12)
    for x in range(x0 + width // 6, x1 - width // 6, tooth + gap):
        rect(pixels, x, bottom, x + tooth, y1 - 2, GOLD)
        rect(pixels, x, y1 - 4, x + tooth, y1 - 2, GOLD_DARK)


def make_icon() -> None:
    size = 96
    pixels = canvas(size)
    cartridge(pixels, 16, 8, 64, 80)
    # NES on the label, plus two little buttons and a pad below it.
    text(pixels, "NES", 30, 30, 2, INK)
    rect(pixels, 30, 48, 66, 49, LABEL_EDGE)
    rounded(pixels, 32, 53, 44, 63, 2, BODY_EDGE)
    rounded(pixels, 54, 53, 64, 63, 5, RED)
    rounded(pixels, 46, 53, 52, 63, 2, RED)
    write_png(APP / "icon.png", pixels)


def make_cart() -> None:
    size = 48
    pixels = canvas(size)
    cartridge(pixels, 8, 4, 32, 40)
    text(pixels, "NES", 15, 16, 1, INK)
    rect(pixels, 15, 25, 33, 26, LABEL_EDGE)
    rounded(pixels, 16, 29, 24, 35, 1, BODY_EDGE)
    rounded(pixels, 27, 29, 33, 35, 3, RED)
    write_png(APP / "cart.png", pixels)


def chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", checksum))


def write_png(path: Path, pixels) -> None:
    height = len(pixels)
    width = len(pixels[0])
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for pixel in row:
            raw.extend(pixel)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)
    print(f"wrote {path} ({width}x{height})")


if __name__ == "__main__":
    make_icon()
    make_cart()
