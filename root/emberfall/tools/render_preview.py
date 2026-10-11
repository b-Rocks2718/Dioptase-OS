#!/usr/bin/env python3
"""Host rendering of Emberfall's arena art for README preview.png.

Approximates the guest's procedural meadow and sprite palette without
requiring PIL. Writes a 2x 640x472 PNG using only the standard library.
"""
import random
import struct
import zlib

W, H = 320, 236


def bgr12(c):
    r = (c & 0xF) * 17
    g = ((c >> 4) & 0xF) * 17
    b = ((c >> 8) & 0xF) * 17
    return (r, g, b)


def circle(px, cx, cy, r, c):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r and 0 <= x < W and 0 <= y < H:
                px[y][x] = c


def rect(px, x0, y0, w, h, c):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            if 0 <= x < W and 0 <= y < H:
                px[y][x] = c


def main():
    rng = random.Random(1234)
    meadow = bgr12(0x242)
    px = [[meadow for _ in range(W)] for _ in range(H)]
    for y in range(H):
        for x in range(W):
            r = rng.random()
            if r < 0.12:
                px[y][x] = bgr12(0x232)
            elif r > 0.97:
                px[y][x] = bgr12(0x353)
    for _ in range(60):
        x, y = rng.randrange(16, 304), rng.randrange(16, 220)
        c = bgr12(rng.choice([0xF44, 0xFF0, 0xFFF]))
        px[y][x] = c
    for _ in range(24):
        x, y = rng.randrange(16, 304), rng.randrange(16, 220)
        rect(px, x, y, 2, 2, bgr12(0x888))
    # Gems.
    for gx, gy in [(60, 60), (120, 180), (200, 90), (260, 160), (90, 120)]:
        c = bgr12(0x5BF)
        px[gy][gx] = c
        px[gy][gx + 1] = bgr12(0xFFF)
        px[gy + 1][gx] = c
        px[gy + 1][gx + 1] = c
    # Enemies around the edges.
    circle(px, 40, 40, 5, bgr12(0x3C3))
    circle(px, 280, 50, 4, bgr12(0xC2E))
    circle(px, 50, 190, 5, bgr12(0xDDD))
    circle(px, 270, 190, 5, bgr12(0x4A2))
    circle(px, 160, 30, 4, bgr12(0x7BE))
    circle(px, 160, 210, 7, bgr12(0x888))
    circle(px, 250, 120, 10, bgr12(0x419))
    circle(px, 250, 120, 7, bgr12(0x61B))
    # Projectiles streaking outward.
    for bx, by in [(140, 100), (180, 110), (150, 130), (170, 90)]:
        rect(px, bx, by, 3, 3, bgr12(0xFF0))
    # Warden at the center.
    circle(px, 160, 118, 5, bgr12(0xA32))
    circle(px, 160, 117, 3, bgr12(0xF9B))
    # Sparks.
    for _ in range(40):
        x, y = rng.randrange(0, W), rng.randrange(0, H)
        px[y][x] = bgr12(rng.choice([0xF80, 0xFF0, 0xF00, 0x0F0]))
    # Scale 2x and write PNG.
    out_w, out_h = W * 2, H * 2
    raw = bytearray()
    for y in range(H):
        for _ in range(2):
            raw.append(0)
            for x in range(W):
                r, g, b = px[y][x]
                raw.extend((r, g, b, r, g, b))

    def chunk(typ, data):
        out = struct.pack(">I", len(data)) + typ + data
        out += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        return out

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open("preview.png", "wb") as f:
        f.write(png)
    print("wrote preview.png", out_w, "x", out_h)


if __name__ == "__main__":
    main()
