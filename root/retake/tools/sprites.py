"""Character sprite sheets for RETAKE.

The stunt double is the only thing on the lot that is in color: she was never
captured on film. Frames are hand-drawn as ASCII art facing right, at most
16x22 pixels, and placed on 32x32 hardware-sprite canvases so the feet sit on
FEET_Y and the body's horizontal center on CENTER_X (the engine aligns the
8-pixel-wide body hitbox to those).
"""
import numpy as np

import pix
from pix import rgb

CANVAS = 32
FEET_Y = 27      # canvas row just below the soles
CENTER_X = 16    # canvas column the body is centered on

HERO_PALETTE = {
    "o": rgb(2, 1, 1),     # outline
    "s": rgb(15, 11, 8),   # skin
    "S": rgb(12, 8, 6),    # skin shadow
    "h": rgb(10, 4, 2),    # auburn hair
    "c": rgb(7, 4, 2),     # leather cap
    "C": rgb(10, 6, 3),    # cap highlight
    "G": rgb(13, 10, 3),   # brass goggle rim
    "g": rgb(9, 14, 15),   # goggle glass
    "r": rgb(15, 3, 2),    # scarf
    "R": rgb(10, 1, 1),    # scarf shadow
    "j": rgb(8, 6, 3),     # flight jacket
    "J": rgb(5, 4, 2),     # jacket shadow
    "k": rgb(11, 8, 4),    # jacket highlight
    "p": rgb(13, 12, 9),   # jodhpurs
    "P": rgb(10, 9, 6),    # jodhpur shadow
    "b": rgb(4, 2, 1),     # boots
    "B": rgb(7, 4, 2),     # boot highlight
    "w": rgb(15, 15, 15),  # clapper stripe / white
    "x": rgb(1, 1, 1),     # clapper black
}

# Frames face right. Each is a list of equal-length rows; '.' is transparent.
HERO_FRAMES = {
    "idle0": [
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "....ohsssSo...",
        "...orrrrrro...",
        "..orRrkjjjjo..",
        "..orRojjjkjo..",
        "..oRo.ojjjso..",
        "......oJjJo...",
        ".....oppPpo...",
        ".....oppPPo...",
        ".....opPoPo...",
        ".....opo.po...",
        ".....obo.bo...",
        "....obbo.bbo..",
        "....ooooooooo.",
    ],
    "idle1": [
        "..............",
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "...orrrrrrro..",
        "..orRrkjjjjo..",
        "..orRojjjkjo..",
        "..oRo.ojjjso..",
        "......oJjJo...",
        ".....oppPpo...",
        ".....opPPPo...",
        ".....opPoPo...",
        ".....opo.po...",
        ".....obo.bo...",
        "....obbo.bbo..",
        "....ooooooooo.",
    ],
    "run0": [
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "....ohsssSo...",
        ".orrrrrrrro...",
        "orRRrkjjjjjo..",
        "oRo.ojjjjkso..",
        ".....oJjjjo...",
        ".....oppPPo...",
        "....oppPoPPo..",
        "...oppo..oPpo.",
        "..obbo....obo.",
        "..obo.....obbo",
        "..oo......oooo",
        "..............",
        "..............",
    ],
    "run1": [
        "..............",
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        ".orrrrrrrro...",
        "orRRrkjjjjo...",
        "oRo.ojjjkjso..",
        ".....oJjjjo...",
        ".....oppPPo...",
        ".....oppoPo...",
        "....oppo.Po...",
        "....obo..obo..",
        "...obbo..obbo.",
        "...oooo..oooo.",
        "..............",
        "..............",
    ],
    "run2": [
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "....ohsssSo...",
        ".orrrrrrrro...",
        "orRRrkjjjjjo..",
        "oRo.ojjjjkso..",
        ".....oJjjjo...",
        ".....oPPppo...",
        "....oPPoPppo..",
        "...oPpo..oppo.",
        "..obo.....obo.",
        ".obbo.....obo.",
        ".oooo......oo.",
        "..............",
        "..............",
    ],
    "jump": [
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "....ohsssSoso.",
        "..orrrrrrrso..",
        ".orRRkjjjjo...",
        ".oRooJjjjko...",
        "......oJjjo...",
        ".....oppPPo...",
        ".....oppoppo..",
        "....oppo.oppo.",
        "....obo...obo.",
        "....obo...obbo",
        "....oo.....ooo",
        "..............",
        "..............",
    ],
    "fall": [
        "..............",
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "..oohsssssSoo.",
        ".osorrrrrrros.",
        "..orRRkjjjjoo.",
        "...orojjjkjo..",
        "......oJjjo...",
        ".....oppPPo...",
        "....oppo.ppo..",
        "...opPo..oPpo.",
        "...obo....obo.",
        "...obbo...obbo",
        "...oooo...oooo",
        "..............",
        "..............",
    ],
    "clap0": [
        ".....oooo.....",
        "....occCCo....",
        "...occcccCo...",
        "...occccGGGo..",
        "...ohccGgggGo.",
        "...ohhsoGGGo..",
        "...ohsssssSo..",
        "....ohsssSo...",
        "...orrrrrro.....",
        "..orRrkjjjjosxxx",
        "..orRojjjkjjowwx",
        "..oRo.ojjjo.xxxx",
        "......oJjJo.....",
        ".....oppPpo.....",
        ".....oppPPo.....",
        ".....opPoPo.....",
        ".....opo.po.....",
        ".....obo.bo.....",
        "....obbo.bbo....",
        "....ooooooooo...",
    ],
    "clap1": [
        ".....oooo.......",
        "....occCCo......",
        "...occcccCo.....",
        "...occccGGGo....",
        "...ohccGgggGo...",
        "...ohhsoGGGo....",
        "...ohsssssSo....",
        "....ohsssSo.....",
        "...orrrrrro..x..",
        "..orRrkjjjjosxwx",
        "..orRojjjkjjowxw",
        "..oRo.ojjjo..xxx",
        "......oJjJo.....",
        ".....oppPpo.....",
        ".....oppPPo.....",
        ".....opPoPo.....",
        ".....opo.po.....",
        ".....obo.bo.....",
        "....obbo.bbo....",
        "....ooooooooo...",
    ],
    "wall": [
        "......oooo....",
        ".....occCCo...",
        "....occcccCo..",
        "....occccGGGo.",
        "....ohccGgggGo",
        "....ohhsoGGGo.",
        "....ohsssssSo.",
        ".....ohsssSos.",
        "...orrrrrrros.",
        "..orRRkjjjjo..",
        ".oRo.ojjjkjo..",
        "......oJjjo...",
        "......oppPo...",
        "......opPPo...",
        ".....oppo.....",
        ".....oPpo.....",
        "....obbo......",
        "....obbbo.....",
        "....ooooo.....",
        "..............",
    ],
    "glide": [
        "...oooooooooo...",
        ".ooxxxxxxxxxxoo.",
        "oxxwxxxxxxxxxxxo",
        "oxxxxxxxxxxxxxxo",
        "o.o.o..o..o.o.oo",
        ".......o........",
        ".....oooo.o.....",
        "....occCCoo.....",
        "...occcccCo.....",
        "...occccGGGo....",
        "...ohccGgggGo...",
        "...ohhsoGGGo....",
        "...ohsssssSo....",
        "....ohsssSo.....",
        "..orrrrrrro.....",
        ".orRRkjjjjo.....",
        "..oRojjjkjo.....",
        "....oJjjjo......",
        "....oppPPo......",
        "....oppoPPo.....",
        "...opPo.oPpo....",
        "...obbo..obbo...",
        "...oooo..oooo...",
    ],
    "hurt": [
        "..............",
        "....oooo......",
        "...occCCo.....",
        "..occcccCo....",
        "..occccGGGo...",
        "..ohccGgggGo..",
        "..ohhsoGGGo...",
        "..ohsososSo...",
        "...orrrrro....",
        "..orRkjjjjo...",
        ".orRojjjkjoo..",
        "..oo.ojjjoso..",
        "....oppPPo....",
        "...oppooPPo...",
        "..oppo..oPpo..",
        "..obo....obo..",
        ".obbo....obbo.",
        ".oooo....oooo.",
        "..............",
        "..............",
    ],
}


def frame_canvas(rows):
    """Place an ASCII frame on a 32x32 transparent canvas, feet on FEET_Y."""
    art = pix.parse_ascii(rows, HERO_PALETTE)
    h, w = art.shape
    # trim empty bottom rows so the feet land exactly on FEET_Y
    while h > 0 and (art[h - 1] == pix.TRANSPARENT).all():
        h -= 1
    art = art[:h]
    canvas = pix.new_image(CANVAS, CANVAS)
    x0 = CENTER_X - 7
    y0 = FEET_Y - h
    pix.blit(canvas, art, x0, y0)
    return canvas


HERO_ORDER = ["idle0", "idle1", "run0", "run1", "run2", "jump", "fall", "clap0", "clap1", "wall", "hurt", "glide"]


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
    hero = [frame_canvas(HERO_FRAMES[name]) for name in HERO_ORDER]
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
