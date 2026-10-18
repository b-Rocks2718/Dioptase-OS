"""Region 3 art: "The Crimson Tide", a pirate swashbuckler on blue night stock.

Night scenes in silent pictures were tinted blue, so this stage is a cool
moonlit monochrome: tarred hull planking, rope-wrapped spars, rope-ladder
rungs, sea urchins as the hazard, cabin paneling for back walls, and a
moonlit sea behind. Falling off a ship is a fall into the sea (a pit).
Tile names match region1.py so rooms.compose works unchanged.
"""
import math

import numpy as np

import pix
import region1
from pix import TRANSPARENT

T = 8


def b(level):
    """Blue night stock."""
    level = max(0, min(15, int(level)))
    return pix.rgb(max(0, level - 4), max(0, level - 2), min(15, level + 1))


def bluify(img):
    out = img.copy()
    m = img != TRANSPARENT
    lv = (img[m] >> 4) & 15
    out[m] = np.array([b(int(v)) for v in lv], dtype=np.uint16)
    return out


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def hull(rng, v):
    """Tarred hull planks: horizontal boards with treenails."""
    t = tile()
    for y in range(T):
        board = (y + v * 3) // 4
        base = 8 if board % 2 else 7
        for x in range(T):
            t[y, x] = b(base + (1 if rng.rand(10) == 0 else 0))
        if (y + v * 3) % 4 == 3:
            t[y, :] = b(4)
    t[(1 + v) % T, (3 + v * 2) % T] = b(12)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = b(1)
        t[1, :] = b(13)
        t[2, :] = b(10)
    if not down:
        t[T - 1, :] = b(1)
        t[T - 2, :] = b(4)
    if not left:
        t[:, 0] = b(1)
    if not right:
        t[:, T - 1] = b(1)
        t[1:T - 1, T - 2] = np.where(t[1:T - 1, T - 2] == b(1), b(1), b(5))
    if not up:
        t[0, :] = b(1)
    return t


def spar(up, right, down, left):
    """A rope-wrapped yard or spar."""
    t = tile(b(8))
    t[0, :] = b(1)
    t[1, :] = b(12)
    t[2, :] = b(10)
    for x in range(T):
        if (x // 2) % 2 == 0:
            t[3:5, x] = b(6)
        else:
            t[3:5, x] = b(11)
    t[5, :] = b(7)
    t[6, :] = b(5)
    t[7, :] = b(1)
    if not left:
        t[:, 0] = b(1)
    if not right:
        t[:, T - 1] = b(1)
    return t


def rung(rng, variant):
    """A rope-ladder rung (one-way)."""
    t = tile()
    t[0, :] = b(1)
    t[1, :] = b(12)
    t[2, :] = b(9)
    t[3, :] = b(1)
    for y in range(4, T):
        t[y, 0 if variant == 0 else 7] = b(7)
    return t


def urchin(direction):
    t = tile()
    for cx in (2, 5):
        for k in range(8):
            ang = k * math.pi / 4
            for r in range(1, 3):
                x = int(round(cx + r * math.cos(ang)))
                y = int(round(5 + r * math.sin(ang)))
                if 0 <= x < T and 0 <= y < T:
                    t[y, x] = b(13 if r == 2 else 3)
        t[5, cx] = b(2)
    t[T - 1, :] = b(3)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def paneling(rng, v):
    """Dark cabin paneling for back walls."""
    t = tile()
    for y in range(T):
        for x in range(T):
            t[y, x] = b(4 if (x + v) % 8 else 2)
    if v % 2:
        t[3, 2:6] = b(5)
    return t


def sail():
    w, h = 40, 48
    s = pix.new_image(w, h)
    for y in range(h):
        bulge = int(5 * math.sin(math.pi * y / h))
        for x in range(2 + bulge, w - 2 - bulge // 2):
            s[y, x] = b(11 if (x // 6) % 2 else 10)
    s[0, :] = b(6)
    s[h - 1, 2:w - 2] = b(6)
    return pix.outline(s, b(3))


def jolly_roger():
    s = pix.new_image(24, 16, b(1))
    for y in range(3, 9):
        for x in range(9, 15):
            if (x - 11.5) ** 2 + (y - 5.5) ** 2 <= 9:
                s[y, x] = b(14)
    s[5, 10] = b(1)
    s[5, 13] = b(1)
    for i in range(8):
        s[9 + i // 2, 6 + i * 1] = b(14)
        s[9 + i // 2, 17 - i * 1] = b(14)
    return s


def porthole(lit):
    s = pix.new_image(12, 12)
    for y in range(12):
        for x in range(12):
            d = (x - 5.5) ** 2 + (y - 5.5) ** 2
            if d <= 30:
                s[y, x] = b(8)
            if d <= 18:
                s[y, x] = pix.rgb(15, 12, 6) if lit else b(3)
    return s


def wheel():
    s = pix.new_image(24, 24)
    for k in range(8):
        ang = k * math.pi / 4
        for r in range(0, 12):
            x = int(round(11.5 + r * math.cos(ang)))
            y = int(round(11.5 + r * math.sin(ang)))
            if 0 <= x < 24 and 0 <= y < 24:
                s[y, x] = b(7)
    for y in range(24):
        for x in range(24):
            d = (x - 11.5) ** 2 + (y - 11.5) ** 2
            if 49 <= d <= 72:
                s[y, x] = b(9)
            if d <= 4:
                s[y, x] = b(11)
    return s


def anchor():
    s = pix.new_image(16, 24)
    s[2:20, 7:9] = b(6)
    s[4, 4:12] = b(6)
    for x in range(1, 15):
        y = 20 - int(5 * math.sin(math.pi * (x - 1) / 13))
        s[y, x] = b(6)
        s[y - 1, x] = b(5)
    for y in range(0, 3):
        for x in range(6, 10):
            if (x - 7.5) ** 2 + (y - 1.5) ** 2 <= 4:
                s[y, x] = b(8)
    return s


def lighthouse():
    w, h = 24, 72
    s = pix.new_image(w, h)
    for y in range(12, h):
        half = 5 + (y - 12) // 10
        for x in range(12 - half, 12 + half):
            s[y, x] = b(10 if ((y // 8) % 2 == 0) else 6)
    s[4:12, 7:17] = b(3)
    s[5:11, 8:16] = pix.rgb(15, 14, 8)
    s[2:4, 6:18] = b(5)
    return s


def backdrop(width, height, rng):
    img = np.zeros((height, width), dtype=np.uint16)
    horizon = height - 70
    for y in range(height):
        for x in range(width):
            img[y, x] = b(2 + (y * 3) // height)
    for _ in range(120):
        x, y = rng.rand(width), rng.rand(horizon)
        img[y, x] = b(10 + rng.rand(5))
    mx, my = width // 3, 44
    for y in range(height):
        for x in range(width):
            if (x - mx) ** 2 + (y - my) ** 2 <= 220:
                img[y, x] = b(14)
    # the sea: rows of wave highlights, and the moon's glitter path
    for y in range(horizon, height):
        for x in range(width):
            img[y, x] = b(3)
            phase = int(4 * math.sin(x / 9.0 + y * 1.7))
            if (y - horizon) % 6 == 0 and phase > 1:
                img[y, x] = b(7)
            if abs(x - mx) < 6 + (y - horizon) // 3 and (x + y) % 3 == 0:
                img[y, x] = b(12)
    # a distant ship silhouette
    sx = width * 2 // 3
    for y in range(horizon - 30, horizon):
        for x in range(sx - 2, sx + 1):
            img[y, x] = b(1)
    for y in range(horizon - 4, horizon):
        for x in range(sx - 22, sx + 20):
            img[y, x] = b(1)
    for y in range(horizon - 26, horizon - 8):
        for x in range(sx + 1, sx + 1 + (y - horizon + 26) // 2):
            img[y, x] = b(2)
    return img


STAMP_CHARS = {
    "W": "porthole_lit", "w": "porthole_dark", "S": "sail", "F": "jolly_roger", "H": "wheel",
    "K": "anchor", "T": "lighthouse", "L": "lamp",
}


def build(font):
    rng = pix.Rng(1927)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for v in range(2):
            tiles[f"brick_{mask}_{v}"] = edges(hull(rng, v), up, right, down, left)
        tiles[f"girder_{mask}"] = spar(up, right, down, left)
    tiles["plank_0"] = rung(rng, 0)
    tiles["plank_1"] = rung(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = urchin(d)
    for v in range(4):
        tiles[f"facade_{v}"] = paneling(rng, v)
    stamps = {
        "porthole_lit": porthole(True), "porthole_dark": porthole(False), "sail": sail(),
        "jolly_roger": jolly_roger(), "wheel": wheel(), "anchor": anchor(), "lighthouse": lighthouse(),
        "lamp": bluify(region1.lamp_stamp()),
    }
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
