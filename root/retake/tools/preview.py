"""Render room previews over their region backdrops, plus one world map per
region with every room placed at its world position (entities marked), to
check art and connectivity without running the emulator.

Usage: python3 tools/preview.py <outdir> [room-name ...]
"""
import os
import sys

import numpy as np

import pix
import region0
import region1
import region2
import region3
import region4
import region5
import region6
import rooms
import world
from font import GLYPHS

ENT_MARK = {"coin": (15, 13, 2), "walker": (15, 2, 2), "hopper": (15, 2, 2), "flyer": (15, 2, 2),
            "thrower": (15, 2, 2), "charger": (15, 2, 2), "boss": (15, 0, 15), "plate": (2, 15, 2),
            "gate": (2, 15, 2), "door": (2, 8, 15), "mirror": (2, 8, 15), "sign": (8, 8, 15),
            "npc": (8, 8, 15), "reel": (15, 15, 15), "frame": (15, 15, 15), "spring": (2, 15, 15),
            "sandbag": (15, 8, 2), "heart": (15, 6, 8), "ability": (15, 15, 15), "shop": (2, 8, 15),
            "platform": (2, 15, 2)}


def main(outdir, names):
    art = {0: region0.build(GLYPHS), 1: region1.build(GLYPHS), 2: region2.build(GLYPHS), 3: region3.build(GLYPHS), 4: region4.build(GLYPHS), 5: region5.build(GLYPHS), 6: region6.build(GLYPHS)}
    chars = {0: region0.STAMP_CHARS, 1: rooms.STAMP_CHARS, 2: region2.STAMP_CHARS, 3: region3.STAMP_CHARS, 4: region4.STAMP_CHARS, 5: region5.STAMP_CHARS, 6: region6.STAMP_CHARS}
    all_rooms = world.all_rooms()
    regions = sorted(art)
    if names:
        regions = sorted({r.region for r in all_rooms if r.name in names})
    for region in regions:
        rs = [r for r in all_rooms if r.region == region]
        x0 = min(r.wx for r in rs)
        y0 = min(r.wy for r in rs)
        x1 = max(r.wx + r.w for r in rs)
        y1 = max(r.wy + r.h for r in rs)
        canvas = np.full(((y1 - y0) * 8, (x1 - x0) * 8), pix.rgb(1, 1, 2), dtype=np.uint16)
        tiles, stamps, back = art[region]
        for r in rs:
            img = rooms.compose(r, tiles, stamps, chars[region])
            h, w = img.shape
            ox, oy = (r.wx - x0) * 8, (r.wy - y0) * 8
            bh, bw = back.shape
            base = back[np.minimum(np.arange(h)[:, None], bh - 1), (np.arange(w)[None, :] // 2) % bw]
            m = img != pix.TRANSPARENT
            base = np.where(m, img, base)
            canvas[oy:oy + h, ox:ox + w] = base
            canvas[oy, ox:ox + w] = pix.rgb(15, 0, 15)
            canvas[oy:oy + h, ox] = pix.rgb(15, 0, 15)
            for kind, tx, ty, _ in r.entities:
                col = pix.rgb(*ENT_MARK.get(kind, (15, 15, 15)))
                canvas[oy + ty * 8 + 2:oy + ty * 8 + 6, ox + tx * 8 + 2:ox + tx * 8 + 6] = col
            sp = r.spawn()
            if sp:
                canvas[oy + sp[1] * 8:oy + sp[1] * 8 + 8, ox + sp[0] * 8 + 3:ox + sp[0] * 8 + 5] = pix.rgb(15, 3, 2)
            if not names or r.name in names:
                pix.write_png(os.path.join(outdir, f"room_{r.name}.png"), base, scale=2)
        pix.write_png(os.path.join(outdir, f"map_region{region}.png"), canvas, scale=1)
        print(f"region {region}: {len(rs)} rooms, map {canvas.shape[1]}x{canvas.shape[0]}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
