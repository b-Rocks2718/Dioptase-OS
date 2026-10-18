"""Pixel-art toolkit for RETAKE's asset generators (host-side, numpy only).

Images are numpy uint16 arrays of packed display colors in the Dioptase
0x0BGR format (docs/mem_map.md: red bits 3:0, green 7:4, blue 11:8). The
value TRANSPARENT (0xF000) marks see-through pixels, matching the hardware's
sprite and tile convention, so sprite frames can be uploaded unchanged.
"""
import struct
import zlib

import numpy as np

TRANSPARENT = 0xF000


def rgb(r, g, b):
    """Pack 4-bit channels (0..15) into a 0x0BGR display color."""
    return (int(b) << 8) | (int(g) << 4) | int(r)


def rgb8(r, g, b):
    """Pack 8-bit channels, rounding each to the nearest 4-bit level."""
    return rgb((r + 8) // 17, (g + 8) // 17, (b + 8) // 17)


def unpack(c):
    """Return (r, g, b) 4-bit channels of a packed color."""
    return c & 15, (c >> 4) & 15, (c >> 8) & 15


def gray(level, warm=0):
    """A 4-bit gray level; `warm` > 0 tints toward silver-sepia."""
    level = max(0, min(15, int(level)))
    r = min(15, level + (1 if warm and level > 2 else 0))
    b = max(0, level - (1 if warm and level > 4 else 0))
    return rgb(r, level, b)


def new_image(w, h, fill=TRANSPARENT):
    return np.full((h, w), fill, dtype=np.uint16)


def to_rgb8(img, background=(255, 0, 255)):
    """Expand a packed image to (h, w, 3) uint8 for previews."""
    img = np.asarray(img, dtype=np.uint16)
    r = (img & 15).astype(np.uint8) * 17
    g = ((img >> 4) & 15).astype(np.uint8) * 17
    b = ((img >> 8) & 15).astype(np.uint8) * 17
    out = np.stack([r, g, b], axis=-1)
    mask = img == TRANSPARENT
    out[mask] = background
    return out


def _chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path, img, scale=1, background=(255, 0, 255)):
    """Write a packed image (or an (h, w, 3) uint8 array) as an RGB PNG."""
    arr = np.asarray(img)
    if arr.ndim == 2:
        arr = to_rgb8(arr, background)
    if scale > 1:
        arr = arr.repeat(scale, axis=0).repeat(scale, axis=1)
    height, width, _ = arr.shape
    raw = b"".join(b"\x00" + arr[y].astype(np.uint8).tobytes() for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _chunk(b"IDAT", zlib.compress(raw, 9))
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def parse_ascii(rows, palette):
    """Turn rows of palette characters into a packed image.

    `palette` maps each character to a packed color; '.' and ' ' are always
    transparent. All rows must have the same length.
    """
    rows = [r for r in rows]
    width = max(len(r) for r in rows)
    img = new_image(width, len(rows))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch in ". ":
                continue
            if ch not in palette:
                raise ValueError(f"parse_ascii: character {ch!r} at ({x}, {y}) has no palette entry")
            img[y, x] = palette[ch]
    return img


def blit(dst, src, x, y):
    """Copy the opaque pixels of `src` onto `dst` with its top-left at (x, y),
    clipping at the destination edges."""
    h, w = src.shape
    dh, dw = dst.shape
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(dw, x + w), min(dh, y + h)
    if x0 >= x1 or y0 >= y1:
        return
    s = src[y0 - y:y1 - y, x0 - x:x1 - x]
    d = dst[y0:y1, x0:x1]
    m = s != TRANSPARENT
    d[m] = s[m]


def mirror(img):
    return img[:, ::-1].copy()


def outline(img, color, diagonal=False):
    """Add a 1-pixel outline of `color` around the opaque pixels."""
    opaque = img != TRANSPARENT
    grow = np.zeros_like(opaque)
    grow[1:, :] |= opaque[:-1, :]
    grow[:-1, :] |= opaque[1:, :]
    grow[:, 1:] |= opaque[:, :-1]
    grow[:, :-1] |= opaque[:, 1:]
    if diagonal:
        grow[1:, 1:] |= opaque[:-1, :-1]
        grow[1:, :-1] |= opaque[:-1, 1:]
        grow[:-1, 1:] |= opaque[1:, :-1]
        grow[:-1, :-1] |= opaque[1:, 1:]
    out = img.copy()
    out[grow & ~opaque] = color
    return out


class Rng:
    """Small deterministic LCG so generated art is identical on every build."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1103515245 + 12345) & 0xFFFFFFFF
        return self.state >> 8

    def rand(self, n):
        return self.next() % n

    def chance(self, num, den):
        return self.rand(den) < num


def le16(values):
    return struct.pack("<%dH" % len(values), *[int(v) & 0xFFFF for v in values])


def le32(values):
    return struct.pack("<%dI" % len(values), *[int(v) & 0xFFFFFFFF for v in values])
