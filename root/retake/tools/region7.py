"""Region 7 art: "Night City", a rain-soaked film noir.

High-contrast black and white with a cold blue cast, and neon as the only
color: pink and cyan tubes, a few lit windows. Ground is wet rooftop brick
whose top edge catches the light, girders are riveted I-beams, planks are
fire-escape grating, the hazard is broken glass, and back walls are dark
brick with the odd lit window. Tile names match region1.py.
"""
import math

import numpy as np

import pix
from pix import TRANSPARENT

T = 8
rgb = pix.rgb


def n(level):
    """Night gray: monochrome with a faint blue cast."""
    level = max(0, min(15, int(level)))
    return rgb(level, level, min(15, level + 1))


PINK = rgb(15, 4, 11)
CYAN = rgb(4, 15, 15)
LAMP = rgb(15, 14, 9)


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def brick(rng, var):
    """Dark brick courses with pale mortar."""
    t = tile(n(4))
    for y in range(T):
        for x in range(T):
            if y % 4 == 3 or (x + (4 if (y // 4 + var) % 2 else 0)) % 8 == 0:
                t[y, x] = n(7)
            elif rng.rand(7) == 0:
                t[y, x] = n(3)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = n(0)
        t[1, :] = n(13)          # the wet edge catches the light
        t[2, :] = n(8)
    if not down:
        t[T - 1, :] = n(0)
    if not left:
        t[:, 0] = n(0)
    if not right:
        t[:, T - 1] = n(0)
    if not up:
        t[0, :] = n(0)
    return t


def ibeam(up, right, down, left):
    t = tile(n(5))
    t[0, :] = n(0)
    t[1, :] = n(11)
    t[2, :] = n(7)
    t[3:6, :] = n(3)
    for x in range(1, T, 4):
        t[4, x] = n(9)
    t[6, :] = n(6)
    t[7, :] = n(0)
    if not left:
        t[:, 0] = n(0)
    if not right:
        t[:, T - 1] = n(0)
    return t


def fire_escape(variant):
    t = tile()
    t[0, :] = n(0)
    t[1, :] = n(10)
    for x in range(T):
        if (x + variant) % 3 == 0:
            t[2:4, x] = n(6)
    t[4, :] = n(0)
    return t


def glass(direction):
    """Broken glass shards, glinting."""
    t = tile()
    for x0, top in ((0, 2), (3, 0), (5, 3)):
        for y in range(top, T):
            half = (y - top) // 3
            for dx in range(-half, half + 1):
                x = x0 + 1 + dx
                if 0 <= x < T:
                    t[y, x] = n(14) if dx <= 0 else n(9)
    t[T - 1, :] = n(2)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def wall(rng, var):
    """Building wall: near-black brick courses (windows are stamps, so the
    texture itself stays quiet)."""
    t = tile(n(2))
    for x in range(T):
        t[3 if (x // 4 + var) % 2 else 7, x] = n(3)
    if var == 3:
        t[1:7, 6] = n(1)         # a downpipe seam
    return t


def neon_glass():
    """A neon cocktail glass in pink tube, 24x24."""
    s = pix.new_image(24, 24)
    for y in range(2, 12):
        half = 10 - (y - 2)
        if half >= 0:
            s[y, 12 - half] = PINK
            s[y, 11 + half] = PINK
    s[2, 2:22] = PINK
    s[12:20, 11:13] = PINK
    s[20, 6:18] = PINK
    for (x, y) in ((15, 6), (16, 5), (17, 6)):
        s[y, x] = CYAN
    return pix.outline(s, rgb(5, 1, 4))


def neon_arrow():
    """A cyan neon arrow pointing right, 32x16, drawn as one tube outline."""
    s = pix.new_image(32, 16)
    s[5, 2:23] = CYAN
    s[10, 2:23] = CYAN
    s[5:11, 2] = CYAN
    s[1:6, 22] = CYAN
    s[10:15, 22] = CYAN
    for k in range(7):
        s[1 + k, 23 + k] = CYAN
        s[14 - k, 23 + k] = CYAN
    return pix.outline(s, rgb(1, 4, 5))


def water_tower():
    """A wooden water tank on stilts, 32x40."""
    s = pix.new_image(32, 40)
    for y in range(0, 8):
        half = 2 + y * 2
        s[y, 16 - half:16 + half] = n(6 if y % 2 else 5)
    s[8:24, 2:30] = n(5)
    for x in range(2, 30, 4):
        s[8:24, x] = n(3)
    s[12, 2:30] = n(8)
    s[20, 2:30] = n(8)
    for x in (5, 26):
        s[24:40, x:x + 2] = n(4)
    for y in range(24, 40, 5):
        s[y, 5:28] = n(4)
    return pix.outline(s, n(0))


def chimney():
    s = pix.new_image(16, 24)
    s[4:24, 3:13] = n(5)
    for y in range(4, 24, 3):
        s[y, 3:13] = n(7)
    s[2:5, 1:15] = n(8)
    return pix.outline(s, n(0))


def lit_window():
    s = pix.new_image(8, 12, n(1))
    s[1:11, 1:7] = LAMP
    s[1:11, 4] = rgb(11, 9, 5)
    s[5, 1:7] = rgb(11, 9, 5)
    s[2:9, 2] = rgb(15, 15, 13)
    return s


def dark_window():
    s = pix.new_image(8, 12, n(1))
    s[1:11, 1:7] = n(3)
    s[5, 1:7] = n(1)
    return s


def billboard():
    """A movie poster billboard, 48x32: a white face, pink lips, dark hair."""
    s = pix.new_image(48, 32, n(9))
    s[0, :] = n(0)
    s[-1, :] = n(0)
    s[:, 0] = n(0)
    s[:, -1] = n(0)
    s[2:30, 2:46] = n(1)
    for y in range(4, 30):
        for x in range(10, 38):
            d = ((x - 24) / 10.0) ** 2 + ((y - 16) / 12.0) ** 2
            if d <= 1:
                s[y, x] = n(14) if x < 26 else n(11)
            elif d <= 1.5 and y < 20:
                s[y, x] = n(0)
    s[13, 19:23] = n(1)
    s[13, 26:30] = n(1)
    s[22:24, 21:28] = PINK
    for x in range(4, 44, 6):
        s[29, x:x + 3] = n(8)
    return s


def backdrop(width, height, rng):
    """Night sky, a hazy moon, two premiere searchlights, the skyline."""
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        img[y, :] = n(1 + int(3 * y / height))
    mx, my = width // 4, 60
    for y in range(height):
        for x in range(width):
            d = (x - mx) ** 2 + (y - my) ** 2
            if d <= 22 * 22:
                img[y, x] = n(13 if d < 18 * 18 else 9)
    for cx, ang in ((width * 2 // 3, -0.35), (width * 2 // 3 + 60, 0.3)):
        for t in range(0, height - 40):
            y = height - 40 - t
            x = int(cx + math.tan(ang) * t)
            for w in range(-1 - t // 40, 2 + t // 40):
                if 0 <= x + w < width and 0 <= y < height and (x + w + y) % 2 == 0:
                    img[y, x + w] = n(6)
    for k in range(40):
        bw = 14 + rng.rand(20)
        bx = rng.rand(width)
        top = height - 60 - rng.rand(110)
        for y in range(top, height):
            for x in range(bx, min(width, bx + bw)):
                img[y, x] = n(0) if y > top else n(2)
        for wy in range(top + 4, height - 4, 7):
            for wx in range(bx + 2, min(width, bx + bw) - 2, 5):
                if rng.rand(5) == 0:
                    img[wy:wy + 3, wx:wx + 2] = LAMP if rng.rand(3) else n(7)
    return img


def arch_tiles():
    """Dark wood wainscot with a pale rail, a cornice, and brick piers."""
    out = {}
    for var in range(2):
        t = tile(n(3))
        t[0, :] = n(1)
        t[:, 0 if var == 0 else 7] = n(1)
        t[2:6, 2:6] = n(4)
        t[2, 2:6] = n(5)
        out[f"wain_{var}"] = t
    t = tile(n(3))
    t[0, :] = n(10)
    t[1, :] = n(6)
    t[2, :] = n(1)
    out["wain_top"] = t
    t = tile(n(5))
    t[0:2, :] = n(8)
    t[5, :] = n(3)
    t[6:8, :] = n(1)
    for x in range(0, T, 2):
        t[3, x] = n(2)                           # dentils
    out["frieze"] = t
    t = tile(n(4))
    for y in range(T):
        t[y, :] = n(4)
        if y % 4 == 3:
            t[y, :] = n(6)
    t[:, 0] = n(1)
    t[:, 1] = n(6)
    t[:, 7] = n(1)
    out["pilaster"] = t
    return out


STAMP_CHARS = {"N": "neon_glass", "A": "neon_arrow", "T": "water_tower", "H": "chimney",
               "W": "lit_window", "w": "dark_window", "B": "billboard", "L": "lamp"}


def build(font):
    import region1
    rng = pix.Rng(1947)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for var in range(2):
            tiles[f"brick_{mask}_{var}"] = edges(brick(rng, var), up, right, down, left)
        tiles[f"girder_{mask}"] = ibeam(up, right, down, left)
    tiles["plank_0"] = fire_escape(0)
    tiles["plank_1"] = fire_escape(1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = glass(d)
    for var in range(4):
        tiles[f"facade_{var}"] = wall(rng, var)
    tiles.update(arch_tiles())
    lamp = region1.lamp_stamp()
    m = lamp != TRANSPARENT
    lamp[m] = np.array([n(int((c >> 4) & 15)) for c in lamp[m]], dtype=np.uint16)
    stamps = {"neon_glass": neon_glass(), "neon_arrow": neon_arrow(), "water_tower": water_tower(),
              "chimney": chimney(), "lit_window": lit_window(), "dark_window": dark_window(),
              "billboard": billboard(), "lamp": lamp}
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
