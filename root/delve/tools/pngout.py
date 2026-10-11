"""Minimal PNG writer for asset previews (no third-party imaging libraries).

The asset generator only needs to show its output to a human, so this writes
8-bit RGB PNGs from a numpy array of shape (height, width, 3).
"""
import struct
import zlib

import numpy as np


def _chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path, rgb):
    """Write an (h, w, 3) uint8 array as an RGB PNG."""
    rgb = np.asarray(rgb, dtype=np.uint8)
    height, width, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _chunk(b"IDAT", zlib.compress(raw, 9))
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
