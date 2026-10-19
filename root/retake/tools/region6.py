"""Region 6 art: "Rocket Rangers of the Moon", a three-strip Technicolor serial.

Saturated, slightly lurid color: the moon is painted rose and lilac, the sky
is a violet gradient with a ringed planet, gantries are chrome with a red
stripe, catwalks are teal grating, and the hazards are cyan crystals. Back
walls are riveted rocket-hull panels. Tile names match region1.py.
"""
import math

import numpy as np

import pix
from pix import TRANSPARENT

T = 8
rgb = pix.rgb

ROCK = (rgb(12, 8, 11), rgb(10, 6, 10), rgb(13, 10, 12))
ROCK_DARK = rgb(6, 3, 7)
ROCK_RIM = rgb(15, 13, 15)
OUTLINE = rgb(2, 0, 3)


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def moonrock(rng, var):
    """Dusty rose regolith; variant 0 carries a small crater."""
    t = tile(ROCK[0])
    for y in range(T):
        for x in range(T):
            n = rng.rand(9)
            if n == 0:
                t[y, x] = ROCK[1]
            elif n == 1:
                t[y, x] = ROCK[2]
    if var == 0:
        for y in range(2, 6):
            for x in range(1, 6):
                d = (x - 3) ** 2 + (y - 3.5) ** 2
                if d <= 3.2:
                    t[y, x] = ROCK_DARK if d <= 1.6 else ROCK[1]
        t[5, 3:5] = ROCK_RIM
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = OUTLINE
        t[1, :] = ROCK_RIM
        t[2, :] = rgb(14, 11, 13)
    if not down:
        t[T - 1, :] = OUTLINE
        t[T - 2, :] = ROCK_DARK
    if not left:
        t[:, 0] = OUTLINE
    if not right:
        t[:, T - 1] = OUTLINE
    if not up:
        t[0, :] = OUTLINE
    return t


def gantry(up, right, down, left):
    """Chrome girder with a red racing stripe and rivets."""
    t = tile(rgb(10, 10, 12))
    t[0, :] = OUTLINE
    t[1, :] = rgb(15, 15, 15)
    t[3:5, :] = rgb(14, 2, 3)
    for x in range(1, T, 3):
        t[2, x] = rgb(6, 6, 8)
        t[5, x] = rgb(6, 6, 8)
    t[6, :] = rgb(6, 6, 8)
    t[7, :] = OUTLINE
    if not left:
        t[:, 0] = OUTLINE
    if not right:
        t[:, T - 1] = OUTLINE
    return t


def grating(variant):
    """Teal catwalk grating, one-way from below."""
    t = tile()
    t[0, :] = OUTLINE
    t[1, :] = rgb(6, 15, 14)
    for x in range(T):
        t[2, x] = rgb(2, 10, 10) if (x + variant) % 2 else TRANSPARENT
    t[3, :] = rgb(1, 6, 7)
    t[4, :] = OUTLINE
    return t


def crystals(direction):
    """Two cyan crystal shards, lit from the left."""
    t = tile()
    for x0, top in ((0, 1), (4, 3)):
        for y in range(top, T):
            half = (y - top) // 2
            for dx in range(-half, half + 1):
                x = x0 + 2 + dx
                if 0 <= x < T:
                    t[y, x] = rgb(12, 15, 15) if dx < 0 else (rgb(4, 13, 14) if dx == 0 else rgb(2, 8, 11))
    t[T - 1, :] = rgb(9, 2, 9)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def hull(var):
    """Rocket-hull back panel: quiet plates with a seam, a rivet pair, and
    in one variant a dim status lamp."""
    t = tile(rgb(2, 4, 6))
    t[0, :] = rgb(1, 3, 5)
    t[1:, 0] = rgb(3, 5, 7)
    if var % 2:
        t[2, 2] = rgb(4, 6, 8)
        t[2, 5] = rgb(4, 6, 8)
    if var == 3:
        t[5, 5] = rgb(9, 2, 3)
    return t


def rocket():
    """The Rangers' rocket on its pad, 24x56, red and cream with fins."""
    w, h = 24, 56
    s = pix.new_image(w, h)
    for y in range(h):
        for x in range(w):
            cx = x - 11.5
            if y < 18:
                half = 1 + y * 5 // 18          # the nose cone
                if abs(cx) <= half:
                    s[y, x] = rgb(14, 13, 11) if cx < 0 else rgb(11, 10, 9)
            elif y < 46:
                if abs(cx) <= 6:
                    band = ((y - 18) // 7) % 2
                    s[y, x] = (rgb(14, 2, 2) if cx < 0 else rgb(10, 1, 2)) if band == 0 else rgb(14, 13, 11)
            if y >= 36:
                fin = (y - 36) // 2 + 6
                if 6 < abs(cx) <= fin and abs(cx) <= 11:
                    s[y, x] = rgb(14, 2, 2)
            if 46 <= y < 50 and abs(cx) <= 4:
                s[y, x] = rgb(6, 6, 8)
    for y in range(24, 31):
        for x in range(w):
            if (x - 11.5) ** 2 + (y - 27) ** 2 <= 7:
                s[y, x] = rgb(4, 13, 14)
    s[26, 10] = rgb(15, 15, 15)
    return pix.outline(s, OUTLINE)


def radar():
    """A radar dish on a lattice mast, 24x24."""
    s = pix.new_image(24, 24)
    for y in range(2, 14):
        for x in range(2, 22):
            if ((x - 12) / 9.5) ** 2 + ((y - 4) / 8.0) ** 2 <= 1 and y >= 4:
                s[y, x] = rgb(13, 13, 14) if x < 12 else rgb(9, 9, 11)
    s[1:5, 11:13] = rgb(14, 2, 3)
    for y in range(12, 23):
        s[y, 11:13] = rgb(7, 7, 9)
        if y % 3 == 0:
            s[y, 10:14] = rgb(10, 10, 12)
    s[22:24, 7:17] = rgb(6, 6, 8)
    return pix.outline(s, OUTLINE)


def alien_plant():
    """A bulbous magenta moon plant with yellow spots, 16x24."""
    s = pix.new_image(16, 24)
    for y in range(10, 24):
        s[y, 7:9] = rgb(4, 11, 6)
    for cx, cy, r in ((8, 6, 5.5), (3.5, 13, 3), (12.5, 11, 3)):
        for y in range(24):
            for x in range(16):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    s[y, x] = rgb(14, 3, 12) if x < cx else rgb(10, 2, 10)
    for x, y in ((6, 4), (9, 7), (7, 9), (3, 12), (12, 10)):
        s[y, x] = rgb(15, 14, 3)
    return pix.outline(s, OUTLINE)


def console():
    """A control console, 32x16, with lamps and two dials."""
    s = pix.new_image(32, 16)
    s[4:16, 1:31] = rgb(4, 6, 9)
    s[1:5, 3:29] = rgb(6, 9, 12)
    s[4, 1:31] = rgb(9, 12, 14)
    lamps = (rgb(15, 3, 2), rgb(15, 14, 3), rgb(4, 15, 6), rgb(4, 13, 15))
    for i, x in enumerate(range(4, 28, 3)):
        s[2, x] = lamps[i % 4]
    for cx in (9, 22):
        for y in range(6, 14):
            for x in range(32):
                if (x - cx) ** 2 + (y - 10) ** 2 <= 9:
                    s[y, x] = rgb(14, 13, 11)
        s[8:11, cx] = OUTLINE
    return pix.outline(s, OUTLINE)


def porthole():
    """A round port onto the stars, 16x16."""
    s = pix.new_image(16, 16)
    for y in range(16):
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if d <= 56:
                s[y, x] = rgb(10, 10, 12) if d > 30 else rgb(1, 0, 4)
    for x, y in ((6, 5), (9, 8), (5, 10), (10, 4)):
        s[y, x] = rgb(15, 15, 13)
    return pix.outline(s, OUTLINE)


def backdrop(width, height, rng):
    """Violet-to-magenta sky, stars, a ringed planet, and jagged peaks."""
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        k = y / height
        img[y, :] = rgb(2 + int(10 * k * k), int(2 * k), 5 + int(5 * k))
    for _ in range(260):
        x, y = rng.rand(width), rng.rand(height * 2 // 3)
        img[y, x] = rgb(15, 15, 12) if rng.rand(3) else rgb(9, 13, 15)
    cx, cy, r = width * 2 // 3, 74, 34
    for y in range(height):
        for x in range(width):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r:
                band = int((dy + r) / 7) % 3
                img[y, x] = (rgb(15, 9, 3), rgb(13, 6, 2), rgb(3, 11, 11))[band]
                if dx + dy > r * 0.6:
                    img[y, x] = rgb(7, 3, 4)
    for t in range(720):
        a = t * 2 * math.pi / 720
        for rr in (r + 12, r + 14, r + 17):
            x = int(cx + rr * 1.6 * math.cos(a))
            y = int(cy + rr * 0.35 * math.sin(a) - 0.25 * rr * math.cos(a))
            behind = math.sin(a) < 0 and (x - cx) ** 2 + (y - cy) ** 2 <= r * r
            if 0 <= x < width and 0 <= y < height and not behind:
                img[y, x] = rgb(15, 13, 9) if rr != r + 14 else rgb(10, 7, 6)
    for x in range(width):
        peak = height - 70 + int(26 * abs(math.sin(x / 23.0)) + 12 * math.sin(x / 7.3))
        for y in range(max(0, peak), height):
            img[y, x] = rgb(4, 1, 6) if y > peak + 2 else rgb(11, 6, 12)
    for x in range(width):
        near = height - 34 + int(9 * math.sin(x / 41.0 + 1.3))
        for y in range(max(0, near), height):
            img[y, x] = rgb(7, 3, 8) if y > near + 1 else rgb(14, 10, 14)
    return img


def arch_tiles():
    """Machinery wainscot with a hazard-striped rail, a pipe run under the
    ceiling, and ribbed struts."""
    out = {}
    for var in range(2):
        t = tile(rgb(3, 5, 7))
        t[0, :] = rgb(1, 2, 4)
        for y in (2, 4, 6):
            t[y, 1:7] = rgb(1, 2, 3)              # vent slots
        if var:
            t[1, 6] = rgb(4, 15, 6)
        out[f"wain_{var}"] = t
    t = tile(rgb(2, 3, 5))
    for x in range(T):
        for y in range(3):
            t[y, x] = rgb(14, 12, 2) if ((x + y) // 2) % 2 == 0 else rgb(2, 2, 2)
    t[3, :] = rgb(1, 1, 2)
    out["wain_top"] = t
    t = tile(rgb(2, 4, 6))
    t[1:6, :] = rgb(8, 9, 11)
    t[2, :] = rgb(13, 14, 15)
    t[5, :] = rgb(4, 5, 7)
    t[1:6, 3] = rgb(5, 6, 8)                     # a coupling
    out["frieze"] = t
    t = tile(rgb(5, 7, 9))
    t[:, 0] = rgb(1, 2, 3)
    t[:, 7] = rgb(1, 2, 3)
    t[:, 1] = rgb(9, 11, 13)
    for y in range(0, T, 2):
        t[y, 2:7] = rgb(4, 6, 8)
    out["pilaster"] = t
    return out


STAMP_CHARS = {"K": "rocket", "D": "radar", "Y": "plant", "C": "console", "O": "porthole"}


def build(font):
    rng = pix.Rng(1936)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for var in range(2):
            tiles[f"brick_{mask}_{var}"] = edges(moonrock(rng, var), up, right, down, left)
        tiles[f"girder_{mask}"] = gantry(up, right, down, left)
    tiles["plank_0"] = grating(0)
    tiles["plank_1"] = grating(1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = crystals(d)
    for var in range(4):
        tiles[f"facade_{var}"] = hull(var)
    tiles.update(arch_tiles())
    stamps = {"rocket": rocket(), "radar": radar(), "plant": alien_plant(),
              "console": console(), "porthole": porthole()}
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
