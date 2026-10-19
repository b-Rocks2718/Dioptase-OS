"""Character sprite sheets for RETAKE.

The stunt double is the only thing on the lot that is in color: she was never
captured on film. Her frames are drawn in ruby.py; the takes' sheets are
derived from them here as silver projections.
"""
import numpy as np

import pix
import ruby

CANVAS = ruby.CANVAS
HERO_ORDER = ruby.ORDER

def ghostify(frame, phase):
    """A take's projection of a hero frame: silver monochrome, a solid dark
    outline, and a checkerboard-thinned body so it reads as translucent film.
    The two phases swap the checkerboard so playback shimmers."""
    out = pix.new_image(CANVAS, CANVAS)
    for y in range(CANVAS):
        for x in range(CANVAS):
            c = int(frame[y, x])
            if c == pix.TRANSPARENT:
                continue
            r, g, b = pix.unpack(c)
            lum = (3 * r + 6 * g + b) // 10
            if lum <= 2:
                out[y, x] = pix.gray(5, warm=1)
            elif (x + y + phase) % 2 == 0:
                out[y, x] = pix.gray(10 + lum // 3, warm=1)
    return out


def build():
    """Return [(sheet_name, [32x32 frames])]; mirrored frames are made at
    load time by the game."""
    hero = ruby.build_frames()
    ghost_a = [ghostify(f, 0) for f in hero]
    ghost_b = [ghostify(f, 1) for f in hero]
    return [("hero", hero), ("ghost_a", ghost_a), ("ghost_b", ghost_b)]


def preview(path):
    sheets = build()
    strip = np.concatenate([np.concatenate(frames, axis=1) for _, frames in sheets], axis=0)
    pix.write_png(path, strip, scale=6, background=(110, 100, 90))


if __name__ == "__main__":
    import sys
    preview(sys.argv[1] if len(sys.argv) > 1 else "/tmp/retake-prev/hero.png")
