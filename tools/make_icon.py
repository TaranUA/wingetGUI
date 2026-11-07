#!/usr/bin/env python3
"""Generate src/app.ico (16/24/32/48 px, 32bpp BGRA) with no external deps.

Design: rounded blue tile with a white "download/update" arrow.
Run from the repo root:  python tools/make_icon.py
"""
import math
import os
import struct
import zlib

SIZES = [16, 24, 32, 48]
SS = 4  # supersampling factor for anti-aliasing

BG_TOP = (58, 132, 246)
BG_BOTTOM = (22, 74, 186)
FG = (255, 255, 255)


def inside_round_rect(x, y, w, h, r):
    if x < 0 or y < 0 or x > w or y > h:
        return False
    cx = min(max(x, r), w - r)
    cy = min(max(y, r), h - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def inside_triangle(x, y, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])

    d1 = sign((x, y), a, b)
    d2 = sign((x, y), b, c)
    d3 = sign((x, y), c, a)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def glyph(x, y):
    """Return True if point (normalised 0..1) is part of the arrow."""
    # shaft
    if 0.435 <= x <= 0.565 and 0.20 <= y <= 0.56:
        return True
    # arrow head
    if inside_triangle(x, y, (0.285, 0.525), (0.715, 0.525), (0.5, 0.775)):
        return True
    # tray
    if 0.215 <= x <= 0.785 and 0.845 <= y <= 0.915:
        r = (0.915 - 0.845) / 2.0
        cx = min(max(x, 0.215 + r), 0.785 - r)
        if (x - cx) ** 2 + (y - 0.88) ** 2 <= r * r:
            return True
    return False


def render(size):
    """Return BGRA rows (top-down) for one icon size."""
    radius = size * 0.215
    px = []
    for py in range(size):
        row = []
        for pxx in range(size):
            bg_cov = 0
            fg_cov = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = pxx + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if not inside_round_rect(x, y, size, size, radius):
                        continue
                    bg_cov += 1
                    if glyph(x / size, y / size):
                        fg_cov += 1
            total = SS * SS
            if bg_cov == 0:
                row.append((0, 0, 0, 0))
                continue
            t = py / max(1, size - 1)
            bg = tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3))
            a_fg = fg_cov / total
            a_bg = bg_cov / total
            r = int(bg[0] * (1 - a_fg) + FG[0] * a_fg)
            g = int(bg[1] * (1 - a_fg) + FG[1] * a_fg)
            b = int(bg[2] * (1 - a_fg) + FG[2] * a_fg)
            row.append((b, g, r, int(a_bg * 255)))
        px.append(row)
    return px


def bmp_payload(px, size):
    # BITMAPINFOHEADER with doubled height (XOR + AND masks)
    header = struct.pack(
        "<IiiHHIIiiII",
        40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0,
    )
    body = bytearray()
    for y in range(size - 1, -1, -1):  # bottom-up
        for x in range(size):
            b, g, r, a = px[y][x]
            body += bytes((b, g, r, a))
    # AND mask: 1 bit per pixel, row padded to 4 bytes
    mask_row = ((size + 31) // 32) * 4
    mask = bytearray(mask_row * size)
    return header + bytes(body) + bytes(mask)


def png_payload(px, size):
    """Encode the pixels as a PNG; ICO supports PNG frames from Windows Vista on.

    PNG frames are much smaller than raw BGRA bitmap frames, which keeps the
    executable (and the .ico file itself) small.
    """
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter: none
        for x in range(size):
            b, g, r, a = px[y][x]
            raw += bytes((r, g, b, a))

    def chunk(kind, data):
        crc = zlib.crc32(kind + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "src", "app.ico")

    images = []
    for size in SIZES:
        images.append((size, png_payload(render(size), size)))

    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = bytearray()
    payload = bytearray()
    for size, data in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        payload += data

    with open(out, "wb") as fh:
        fh.write(header + bytes(entries) + bytes(payload))
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()