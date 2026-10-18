"""UI art: the intertitle card frame and the HUD tileset.

The intertitle frame is a 320x236 pixel-layer image: black stock, a double
Art Deco rule with stepped corners and a sunburst crest, and a soft
vignette. The HUD lives on the tile layer at tile scale 0 (8x8 tiles at
640x480), two tile rows tall, so its icons are drawn at twice the game's
pixel density.
"""
import numpy as np

import pix
from font import GLYPHS
from pix import TRANSPARENT, gray, rgb

W, H = 320, 236


def g(level):
    return gray(level, warm=1)


def card_frame():
    img = np.full((H, W), g(1), dtype=np.uint16)
    rng = pix.Rng(1937)
    # faint grain in the black stock
    for _ in range(900):
        x, y = rng.rand(W), rng.rand(H)
        img[y, x] = g(2)
    # vignette: darken the outer band
    for y in range(H):
        for x in range(W):
            d = min(x, y, W - 1 - x, H - 1 - y)
            if d < 6 and (x + y) % (d + 2) == 0:
                img[y, x] = g(0)

    def hline(y, x0, x1, c):
        img[y, x0:x1 + 1] = c

    def vline(x, y0, y1, c):
        img[y0:y1 + 1, x] = c

    def rect(x0, y0, x1, y1, c):
        hline(y0, x0, x1, c)
        hline(y1, x0, x1, c)
        vline(x0, y0, y1, c)
        vline(x1, y0, y1, c)

    rect(10, 10, W - 11, H - 11, g(13))
    rect(13, 13, W - 14, H - 14, g(8))
    # stepped Art Deco corners
    for cx, cy, sx, sy in ((10, 10, 1, 1), (W - 11, 10, -1, 1), (10, H - 11, 1, -1), (W - 11, H - 11, -1, -1)):
        for step in range(3):
            o = 4 + step * 4
            for i in range(o + 1):
                img[cy + sy * o, cx + sx * i] = g(13)
                img[cy + sy * i, cx + sx * o] = g(13)
        for k in range(1, 4):
            img[cy + sy * (k * 4 - 2), cx + sx * (k * 4 - 2)] = g(15)
    # sunburst crest on the top rule, a small diamond on the bottom
    cx = W // 2
    for ray in range(-4, 5):
        for r in range(2, 12):
            x = cx + (ray * r) // 4
            y = 10 + r // 2 if False else 10 - 0
            yy = 10 + r
            if 0 <= x < W and yy < 30:
                if r < 3 + (4 - abs(ray)) * 2:
                    img[yy, x] = g(13 if r % 2 else 10)
    for i in range(6):
        img[10 + i, cx - 6 + i:cx + 7 - i] = g(1)
    for i in range(5):
        img[11 + i, cx - 4 + i:cx + 5 - i] = g(14) if i % 2 == 0 else g(11)
    for i in range(4):
        img[H - 11 - i, cx - 3 + i:cx + 4 - i] = g(13)
        img[H - 11 + i, cx - 3 + i:cx + 4 - i] = g(13)
    return img


# ---------------------------------------------------------------- HUD tiles

def _tile(fill=TRANSPARENT):
    return np.full((8, 8), fill, dtype=np.uint16)


def _icon_frame(full):
    """A 16x16 film-frame health pip: sprocket rails and a lit window."""
    s = pix.new_image(16, 16, rgb(0, 0, 0))
    edge = rgb(14, 13, 11) if full else rgb(5, 5, 4)
    fill = rgb(15, 3, 2) if full else rgb(2, 1, 1)
    for y in range(1, 15):
        s[y, 2] = edge
        s[y, 13] = edge
    for y in range(2, 14, 3):
        s[y, 1] = edge if full else rgb(3, 3, 3)
        s[y, 14] = edge if full else rgb(3, 3, 3)
    s[1, 2:14] = edge
    s[14, 2:14] = edge
    for y in range(3, 13):
        for x in range(4, 12):
            s[y, x] = fill
    if full:
        s[4, 5:9] = rgb(15, 9, 7)
        s[5, 5:7] = rgb(15, 9, 7)
    return s


def _icon_coin():
    s = pix.new_image(16, 16, rgb(0, 0, 0))
    for y in range(16):
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if d <= 36:
                s[y, x] = rgb(14, 11, 3)
            if d <= 20:
                s[y, x] = rgb(15, 13, 6)
    for y in range(5, 11):
        s[y, 7] = rgb(9, 6, 1)
        s[y, 8] = rgb(9, 6, 1)
    s[5, 6:10] = rgb(9, 6, 1)
    s[10, 6:10] = rgb(9, 6, 1)
    return s


def _icon_reel(filled):
    """A film reel: a take slot (filled when a take is playing)."""
    s = pix.new_image(16, 16, rgb(0, 0, 0))
    rim = rgb(13, 13, 14) if filled else rgb(5, 5, 6)
    for y in range(16):
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if 30 <= d <= 49:
                s[y, x] = rim
            elif d < 30 and filled:
                s[y, x] = rgb(9, 9, 11)
    for cx, cy in ((7.5, 4), (4, 9.5), (11, 9.5)):
        for y in range(16):
            for x in range(16):
                if (x - cx) ** 2 + (y - cy) ** 2 <= 3:
                    s[y, x] = rgb(0, 0, 0)
    s[7:9, 7:9] = rim
    return s


def _icon_rec(on):
    s = pix.new_image(16, 16, rgb(0, 0, 0))
    if on:
        for y in range(16):
            for x in range(16):
                if (x - 7.5) ** 2 + (y - 7.5) ** 2 <= 20:
                    s[y, x] = rgb(15, 2, 1)
        s[5, 6] = rgb(15, 10, 9)
    return s


def _split16(img):
    """A 16x16 icon as four 8x8 tiles: top-left, top-right, bottom-left,
    bottom-right."""
    return [img[0:8, 0:8], img[0:8, 8:16], img[8:16, 0:8], img[8:16, 8:16]]


def _glyph_tiles(ch, color):
    """A 5x7 glyph centered in an 8x16 cell (two tiles: top, bottom)."""
    cell = pix.new_image(8, 16, rgb(0, 0, 0))
    rows = GLYPHS.get(ch)
    if rows:
        for gy, row in enumerate(rows):
            for gx, bit in enumerate(row):
                if bit == "#":
                    cell[4 + gy, 1 + gx] = color
    return [cell[0:8, :], cell[8:16, :]]


HUD_TEXT_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,!?'-:/&"


def hud_tiles():
    """Return (tiles, index): tiles is a list of 8x8 images uploaded to the
    tile layer from index 0; index maps names to their first tile."""
    tiles = [_tile(TRANSPARENT), _tile(rgb(0, 0, 0))]
    index = {"clear": 0, "black": 1}

    def add(name, parts):
        index[name] = len(tiles)
        tiles.extend(parts)

    add("frame_full", _split16(_icon_frame(True)))
    add("frame_empty", _split16(_icon_frame(False)))
    add("coin", _split16(_icon_coin()))
    add("reel_full", _split16(_icon_reel(True)))
    add("reel_empty", _split16(_icon_reel(False)))
    add("rec_on", _split16(_icon_rec(True)))
    index["font"] = len(tiles)
    for ch in HUD_TEXT_CHARS:
        tiles.extend(_glyph_tiles(ch, rgb(14, 13, 11)))
    index["font_red"] = len(tiles)
    for ch in "REC0123456789":
        tiles.extend(_glyph_tiles(ch, rgb(15, 3, 2)))
    if len(tiles) > 256:
        raise SystemExit(f"ui.py: HUD tileset needs {len(tiles)} tiles, the tile layer holds 256")
    return tiles, index
