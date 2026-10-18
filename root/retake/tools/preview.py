"""Render every room in the given room files to PNG previews (2x scale) over
the region backdrop, to check art without running the emulator."""
import sys

import numpy as np

import pix
import region1
import rooms
from font import GLYPHS


def main(paths, outdir):
    tiles, stamps, back = region1.build(GLYPHS)
    pix.write_png(f"{outdir}/backdrop_r1.png", back, scale=2)
    for path in paths:
        for room in rooms.parse(path):
            img = rooms.compose(room, tiles, stamps)
            h, w = img.shape
            canvas = np.zeros((h, w), dtype=np.uint16)
            bh, bw = back.shape
            for y in range(h):
                for x in range(w):
                    canvas[y, x] = back[min(y, bh - 1), (x // 2) % bw]
            m = img != pix.TRANSPARENT
            canvas[m] = img[m]
            pix.write_png(f"{outdir}/room_{room.name}.png", canvas, scale=2)
            print(f"{room.name}: {w}x{h}")


if __name__ == "__main__":
    main(sys.argv[2:], sys.argv[1])
