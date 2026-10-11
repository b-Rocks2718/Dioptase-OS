#!/usr/bin/env python3
"""Composite Emberfall e2e framebuffer dumps into a PNG screenshot.

Reads shot_pixel.bin (320x236 LE shorts), shot_tilefb.bin (80x60 cells of
index+RGB332-color), and shot_tilemap.bin (256 8x8 glyph tiles) as written by
`emberfall --e2e`, and composites the 640x480 VGA frame exactly like
Dioptase-Emulator-Full/src/graphics.rs: the pixel layer doubles to fill the
frame, tile cells draw 1:1 over it (0xFxxx transparent, 0xCxxx ink takes the
entry's RGB332 color, anything else is packed 0x0BGR), sprites skipped.
Full 640x480 frame; only the last four arena rows stay black. Stdlib only.
"""

import struct
import sys
import zlib


def bgr12(v):
    return ((v & 0xF) * 16, ((v >> 4) & 0xF) * 16, ((v >> 8) & 0xF) * 16)


def rgb332(c):
    r3 = (c >> 5) & 0x7
    g3 = (c >> 2) & 0x7
    b2 = c & 0x3
    r = (r3 << 1) | (r3 >> 2)
    g = (g3 << 1) | (g3 >> 2)
    b = (b2 << 2) | b2
    return (r * 16, g * 16, b * 16)


def main(dump_dir, out_path):
    px = open(dump_dir + '/shot_pixel.bin', 'rb').read()
    tfb = open(dump_dir + '/shot_tilefb.bin', 'rb').read()
    tm = open(dump_dir + '/shot_tilemap.bin', 'rb').read()
    pixels = struct.unpack('<%dH' % (len(px) // 2), px)
    cells = struct.unpack('<%dH' % (len(tfb) // 2), tfb)
    glyphs = struct.unpack('<%dH' % (len(tm) // 2), tm)
    W, H = 640, 480
    buf = bytearray(W * H * 3)
    # Pixel layer doubled.
    for y in range(236):
        for x in range(320):
            r, g, b = bgr12(pixels[y * 320 + x])
            for dy in range(2):
                base = ((y * 2 + dy) * W + x * 2) * 3
                buf[base:base + 3] = bytes((r, g, b))
                buf[base + 3:base + 6] = bytes((r, g, b))
    # Tile layer 1:1 over it.
    for ty in range(60):
        for tx in range(80):
            cell = cells[ty * 80 + tx]
            tile = (cell & 0xFF) * 64
            r, g, b = rgb332((cell >> 8) & 0xFF)
            for py in range(8):
                for pxx in range(8):
                    raw = glyphs[tile + py * 8 + pxx]
                    if raw >> 12 == 0xF:
                        continue
                    if raw >> 12 == 0xC:
                        pr, pg, pb = r, g, b
                    else:
                        pr, pg, pb = bgr12(raw)
                    o = ((ty * 8 + py) * W + tx * 8 + pxx) * 3
                    buf[o:o + 3] = bytes((pr, pg, pb))
    # Write the full frame as a PNG.
    rows = b''
    for y in range(H):
        rows += b'\x00' + bytes(buf[y * W * 3:(y + 1) * W * 3])

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b''))
    open(out_path, 'wb').write(png)
    print('wrote ' + out_path)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
