"""Region 0 art: the Backlot, the studio lot between the sound stages.

The lot is frozen in silver nitrate: cold blue-silver grays at night, under
searchlights that still sweep for a premiere nobody left to attend. Tile
names match region1.py so rooms.compose can use either set.
"""
import numpy as np

import pix
import region1
from font import GLYPHS
from pix import TRANSPARENT

T = 8


def c(level):
    """Cold silver: gray with a blue lean."""
    level = max(0, min(15, int(level)))
    b = min(15, level + (2 if level > 1 else 1))
    r = max(0, level - 1)
    return pix.rgb(r, level, b)


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def stucco(rng, base):
    t = tile(c(base))
    for y in range(T):
        for x in range(T):
            n = rng.rand(7)
            if n == 0:
                t[y, x] = c(base + 1)
            elif n == 1:
                t[y, x] = c(base - 1)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = c(1)
        t[1, :] = c(13)
        t[2, :] = c(11)
    if not down:
        t[T - 1, :] = c(1)
        t[T - 2, :] = c(5)
    if not left:
        t[:, 0] = c(1)
    if not right:
        t[:, T - 1] = c(1)
        t[1:T - 1, T - 2] = np.where(t[1:T - 1, T - 2] == c(1), c(1), c(6))
    return t


def girder(up, right, down, left):
    """Steel catwalk framing for the stage rigging."""
    t = tile(c(6))
    t[0, :] = c(1)
    t[1, :] = c(12)
    t[2, :] = c(9)
    t[3:6, :] = c(4)
    for x in range(0, T, 4):
        t[3:6, x] = c(8)
        t[3:6, x + 1] = c(7)
    t[6, :] = c(8)
    t[7, :] = c(1)
    if not left:
        t[:, 0] = c(1)
    if not right:
        t[:, T - 1] = c(1)
    return t


def plank(rng, variant):
    t = tile()
    t[0, :] = c(1)
    t[1, :] = c(11)
    t[2, :] = c(9)
    t[3, :] = c(5)
    t[4, :] = c(1)
    if variant:
        t[1:4, 0] = c(3)
    return t


def stage_wall(rng, variant):
    """Sound-stage stucco: big panels with a pilaster every 4 tiles."""
    t = stucco(rng, 5)
    if variant == 0:
        t[:, 0] = c(7)
        t[:, 1] = c(6)
        t[:, 7] = c(3)
    return t


def stage_door(number):
    """A hangar-style stage door with the stage number painted large."""
    w, h = 40, 48
    s = pix.new_image(w, h, c(3))
    s[0:3, :] = c(9)
    s[3:h, 2:w - 2] = c(6)
    for x in range(4, w - 4, 6):
        s[5:h, x] = c(4)
    s[h - 2:h, :] = c(2)
    # number plate: dark disc with the digit
    cx, cy = w // 2, 16
    for y in range(h):
        for x in range(w):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if d <= 100:
                s[y, x] = c(2)
            if d <= 81:
                s[y, x] = c(12)
    glyph = GLYPHS[str(number)]
    for gy, row in enumerate(glyph):
        for gx, bit in enumerate(row):
            if bit == "#":
                for dy in range(2):
                    for dx in range(2):
                        s[cy - 7 + gy * 2 + dy, cx - 5 + gx * 2 + dx] = c(2)
    # "STAGE" on a small lintel plate
    text = "STAGE"
    x0 = (w - len(text) * 6) // 2
    for i, ch in enumerate(text):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "#" and 30 + gy < h:
                    s[30 + gy, x0 + i * 6 + gx] = c(13)
    return s


def palm():
    w, h = 32, 64
    s = pix.new_image(w, h)
    cx = 16
    for y in range(18, h):
        sway = (y - 18) // 14
        for x in range(cx - 2 + sway, cx + 2 + sway):
            s[y, x] = c(3 if (y // 3) % 2 else 4)
    fronds = [(-14, 6), (-10, 11), (14, 6), (10, 11), (-6, 14), (6, 14), (0, 2)]
    for fx, fy in fronds:
        steps = 14
        for k in range(steps):
            x = cx + (fx * k) // steps
            y = 18 + (fy * k) // steps - (k * (steps - k)) // 10
            for dy in range(-1, 2):
                if 0 <= y + dy < h and 0 <= x < w:
                    s[y + dy, x] = c(2)
    return s


def water_tower():
    w, h = 48, 72
    s = pix.new_image(w, h)
    for y in range(4, 34):
        for x in range(6, 42):
            s[y, x] = c(7 if x < 30 else 5)
    for x in range(6, 42):
        s[4, x] = c(10)
    for y in range(0, 5):
        for x in range(10 - y * 0, 38):
            if abs(x - 24) <= 14 - y * 2:
                s[y, x] = c(9)
    text = "SILVERLAKE"
    x0 = 24 - len(text) * 2
    for i, ch in enumerate(text):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "#" and gx < 4:
                    s[15 + gy, x0 + i * 4 + gx] = c(13)
    for y in range(34, h):
        for x in (10, 11, 36, 37, 23, 24):
            s[y, x] = c(3)
        if (y - 34) % 12 == 0:
            s[y, 10:38] = c(3)
    return s


def tower():
    """The Projection Tower: an Art Deco spire, the last place to go."""
    w, h = 56, 120
    s = pix.new_image(w, h)
    for y in range(h):
        half = 8 + (y * 18) // h
        for x in range(28 - half, 28 + half):
            s[y, x] = c(4 if x < 28 else 3)
        if y % 10 == 0:
            s[y, 28 - half:28 + half] = c(7)
    for y in range(20, h, 10):
        for x in range(24, 32):
            s[y + 3:y + 6, x] = c(11) if (x + y) % 3 else c(9)
    for y in range(0, 20):
        s[y, 27:29] = c(12)
    return s


def backdrop(width, height, rng):
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        level = 1 + (y * 4) // height
        for x in range(width):
            img[y, x] = c(level)
    for _ in range(140):
        x, y = rng.rand(width), rng.rand(height * 2 // 3)
        img[y, x] = c(10 + rng.rand(5))
    # moon
    mx, my = width * 3 // 4, 34
    for y in range(height):
        for x in range(width):
            d = (x - mx) ** 2 + (y - my) ** 2
            if d <= 150:
                img[y, x] = c(14)
            elif d <= 190 and (x + y) % 2:
                img[y, x] = c(6)
    # searchlight beams from below the hills
    for bx, slope in ((width // 5, 3), (width // 2, -2), (width * 4 // 5, 2)):
        for y in range(40, height):
            for k in range(-2 - (height - y) // 12, 3 + (height - y) // 12):
                x = bx + (height - y) // slope + k
                if 0 <= x < width and (x + y) % 2 == 0:
                    img[y, x] = c(int(min(9, 4 + (y - 40) // 40)))
    # hills
    for x in range(width):
        top = height - 70 + int(14 * np.sin(x / 37.0) + 8 * np.sin(x / 13.0 + 1))
        for y in range(top, height):
            img[y, x] = c(2)
    # the SILVERLAKE sign on the hillside
    text = "SILVERLAKE"
    x0, y0 = 60, height - 86
    for i, ch in enumerate(text):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "#":
                    for dy in range(2):
                        img[y0 + gy * 2 + dy, x0 + i * 13 + gx * 2] = c(12)
                        img[y0 + gy * 2 + dy, x0 + i * 13 + gx * 2 + 1] = c(12)
    return img


STAMP_CHARS = {
    "1": "door1", "2": "door2", "3": "door3", "4": "door4", "5": "door5", "6": "door6", "7": "door7",
    "8": "door8", "P": "palm", "T": "water_tower", "X": "tower", "L": "lamp", "W": "window_lit",
    "w": "window_dark",
}


def build(font):
    rng = pix.Rng(1937)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for v in range(2):
            tiles[f"brick_{mask}_{v}"] = edges(stucco(rng, 9 + v), up, right, down, left)
        tiles[f"girder_{mask}"] = girder(up, right, down, left)
    tiles["plank_0"] = plank(rng, 0)
    tiles["plank_1"] = plank(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = region1.nails(d)
    for v in range(4):
        tiles[f"facade_{v}"] = stage_wall(rng, v)
    stamps = {f"door{n}": stage_door(n) for n in range(1, 9)}
    stamps.update({
        "palm": palm(), "water_tower": water_tower(), "tower": tower(),
        "lamp": region1.lamp_stamp(),
        "window_lit": region1.window_stamp(True, rng), "window_dark": region1.window_stamp(False, rng),
    })
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
