"""Region 8 art: "Starlight Revue", a full-Technicolor backstage musical.

Warm and gaudy: polished stage boards edged in gold, gilded truss girders,
red-carpeted risers for planks, upturned prop sabres as the hazard, and
red velvet with deep folds for back walls. Behind it all, the auditorium:
gold balcony rails, a chandelier, and an audience that never goes home.
Tile names match region1.py.
"""
import math

import numpy as np

import pix
from pix import TRANSPARENT

T = 8
rgb = pix.rgb
GOLD = rgb(15, 12, 3)
GOLD_DARK = rgb(10, 7, 1)
VELVET = rgb(10, 1, 3)
VELVET_DARK = rgb(5, 0, 2)
OUT = rgb(2, 0, 1)


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def boards(rng, var):
    """Polished stage boards with staggered seams."""
    t = tile(rgb(11, 6, 3))
    for y in range(T):
        for x in range(T):
            if y % 4 == 0:
                t[y, x] = rgb(8, 4, 2)
            elif (x + 3 * (y // 4) + var * 2) % 8 == 0:
                t[y, x] = rgb(7, 3, 1)
            elif rng.rand(9) == 0:
                t[y, x] = rgb(12, 7, 4)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = OUT
        t[1, :] = GOLD
        t[2, :] = GOLD_DARK
    if not down:
        t[T - 1, :] = OUT
    if not left:
        t[:, 0] = OUT
    if not right:
        t[:, T - 1] = OUT
    if not up:
        t[0, :] = OUT
    return t


def truss(up, right, down, left):
    """A gilded truss: two gold chords with a zigzag web."""
    t = tile()
    t[0, :] = OUT
    t[1, :] = GOLD
    t[6, :] = GOLD_DARK
    t[7, :] = OUT
    for x in range(T):
        y = 2 + (x % 4 if (x // 4) % 2 == 0 else 3 - x % 4)
        t[y, x] = GOLD_DARK
    if not left:
        t[:, 0] = OUT
    if not right:
        t[:, T - 1] = OUT
    return t


def riser(variant):
    """A stage riser: a red carpet runner on a wooden lip, one-way."""
    t = tile()
    t[0, :] = OUT
    t[1, :] = rgb(15, 3, 4)
    t[2, :] = rgb(11, 1, 2)
    t[3, :] = rgb(9, 5, 2)
    t[4, :] = OUT
    if variant:
        t[1, 3] = GOLD
    return t


def sabres(direction):
    """Upturned prop sabres: silver blades, gold hilts."""
    t = tile()
    for x0 in (1, 5):
        t[1:6, x0] = rgb(14, 14, 15)
        t[1:6, x0 + 1] = rgb(9, 9, 11)
        t[0, x0] = rgb(15, 15, 15)
        t[6, x0 - 1:x0 + 3] = GOLD
        t[7, x0:x0 + 2] = GOLD_DARK
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def velvet(var):
    """Red velvet with vertical folds; one variant carries a gold tassel."""
    t = tile(VELVET)
    for x in range(T):
        fold = (x + var * 3) % 8
        if fold in (0, 1):
            t[:, x] = VELVET_DARK
        elif fold == 4:
            t[:, x] = rgb(13, 2, 4)
    if var == 3:
        t[2:5, 4] = GOLD
    return t


def star():
    """A gold star, 24x24."""
    s = pix.new_image(24, 24)
    pts = []
    for k in range(10):
        r = 11 if k % 2 == 0 else 4.5
        a = -math.pi / 2 + k * math.pi / 5
        pts.append((11.5 + r * math.cos(a), 12 + r * math.sin(a)))
    for y in range(24):
        for x in range(24):
            inside = False
            j = len(pts) - 1
            for i in range(len(pts)):
                xi, yi = pts[i]
                xj, yj = pts[j]
                if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                    inside = not inside
                j = i
            if inside:
                s[y, x] = GOLD if x + y < 24 else GOLD_DARK
    return pix.outline(s, OUT)


def curtain():
    """A swagged red curtain drape with a gold tieback, 24x56."""
    s = pix.new_image(24, 56)
    for y in range(56):
        width = 24 if y < 30 else max(8, 24 - (y - 30) // 2)
        for x in range(width):
            fold = (x + y // 12) % 6
            s[y, x] = VELVET_DARK if fold == 0 else (rgb(13, 2, 4) if fold == 3 else VELVET)
    s[0:3, :] = GOLD
    s[30:33, 0:20] = GOLD
    return pix.outline(s, OUT)


def paper_moon():
    """A painted paper crescent moon on its wire, 24x24."""
    s = pix.new_image(24, 24)
    for y in range(24):
        for x in range(24):
            if (x - 12) ** 2 + (y - 13) ** 2 <= 100 and (x - 17) ** 2 + (y - 10) ** 2 > 64:
                s[y, x] = rgb(15, 14, 9) if x < 10 else rgb(13, 11, 6)
    s[0:4, 12] = rgb(9, 9, 9)
    s[9, 7] = OUT
    return pix.outline(s, OUT)


def piano():
    """A white grand piano, 40x24."""
    s = pix.new_image(40, 24)
    for y in range(4, 14):
        for x in range(2, 38):
            if x < 26 or (x - 26) ** 2 / 144 + (y - 14) ** 2 / 100 <= 1:
                s[y, x] = rgb(15, 15, 14)
    s[2:5, 4:24] = rgb(13, 13, 12)
    s[13, 2:38] = rgb(9, 9, 9)
    for x in range(4, 24, 2):
        s[11:13, x] = OUT
    for x in (5, 33):
        s[14:24, x:x + 2] = rgb(12, 12, 11)
    return pix.outline(s, OUT)


def backdrop(width, height, rng):
    """The auditorium: dark maroon, gold balcony rails, a chandelier, and
    rows of heads in the stalls."""
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        img[y, :] = rgb(3 + int(4 * y / height), 0, 1)
    for tier, y0 in enumerate((70, 130, 190)):
        for x in range(width):
            yy = y0 + int(6 * math.sin(x / 40.0 + tier))
            img[yy:yy + 2, x] = GOLD_DARK
            if x % 12 == 0:
                img[yy + 2:yy + 10, x] = GOLD_DARK
        for x in range(4, width, 9):
            yy = y0 - 6 + int(6 * math.sin(x / 40.0 + tier))
            for dy in range(5):
                for dx in range(5):
                    if (dx - 2) ** 2 + (dy - 2) ** 2 <= 5:
                        img[yy + dy, x + dx - 2] = rgb(2, 0, 1)
    cx = width // 2
    for y in range(0, 40):
        img[y, cx] = GOLD_DARK
    for k in range(60):
        a = k * 2 * math.pi / 60
        x = int(cx + 26 * math.cos(a))
        y = int(46 + 10 * math.sin(a))
        img[y, x] = GOLD
        if k % 5 == 0:
            img[y + 1:y + 5, x] = rgb(14, 14, 15)
    for x in range(0, width, 7):
        y = height - 18 + (x // 7) % 3
        for dy in range(6):
            for dx in range(6):
                if (dx - 3) ** 2 + (dy - 3) ** 2 <= 8:
                    if 0 <= x + dx < width:
                        img[min(height - 1, y + dy), x + dx] = rgb(1, 0, 1)
    return img


STAMP_CHARS = {"S": "star", "C": "curtain", "M": "paper_moon", "P": "piano", "L": "lamp"}


def build(font):
    import region1
    rng = pix.Rng(1938)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for var in range(2):
            tiles[f"brick_{mask}_{var}"] = edges(boards(rng, var), up, right, down, left)
        tiles[f"girder_{mask}"] = truss(up, right, down, left)
    tiles["plank_0"] = riser(0)
    tiles["plank_1"] = riser(1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = sabres(d)
    for var in range(4):
        tiles[f"facade_{var}"] = velvet(var)
    lamp = region1.lamp_stamp()
    m = lamp != TRANSPARENT
    lamp[m] = np.where(lamp[m] == lamp[m].max(), GOLD, lamp[m])
    stamps = {"star": star(), "curtain": curtain(), "paper_moon": paper_moon(),
              "piano": piano(), "lamp": lamp}
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
