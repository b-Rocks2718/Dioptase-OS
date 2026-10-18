"""Region 4 art: "Jungle Serial, Chapter 12", in two-strip Technicolor.

Two-strip Technicolor could only mix a red-orange record with a blue-green
one, so the jungle has no true blues or yellows: foliage is teal, stone and
skin are salmon and brick, and the sky is a pale peach-green. Ground is
mossy temple stone, girders are lashed bamboo, planks are vine bridges, the
hazard is a bed of thorns, and back walls are carved temple blocks. Tile
names match region1.py so rooms.compose works unchanged.
"""
import math

import numpy as np

import pix
from pix import TRANSPARENT

T = 8


def red(level):
    """The red-orange record."""
    level = max(0, min(15, int(level)))
    return pix.rgb(level, max(0, level * 2 // 3 - 1), max(0, level // 2 - 1))


def teal(level):
    """The blue-green record."""
    level = max(0, min(15, int(level)))
    return pix.rgb(max(0, level // 3), level, max(0, level * 3 // 4))


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def stone(rng, v):
    t = tile()
    for y in range(T):
        for x in range(T):
            base = 8 if ((x // 4 + y // 4 + v) % 2) else 7
            t[y, x] = red(base + (1 if rng.rand(9) == 0 else 0))
    t[3, :] = red(4)
    t[7, :] = red(4)
    t[0:3, (3 + v) % 8] = red(4)
    t[4:7, (7 + v) % 8] = red(4)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:   # a mossy lip
        t[0, :] = teal(2)
        t[1, :] = teal(11)
        t[2, :] = teal(8)
        for x in range(0, T, 3):
            t[3, x] = teal(7)
    if not down:
        t[T - 1, :] = red(1)
        t[T - 2, :] = red(4)
    if not left:
        t[:, 0] = red(1)
    if not right:
        t[:, T - 1] = red(1)
    if not up:
        t[0, :] = teal(2)
    return t


def bamboo(up, right, down, left):
    t = tile(teal(9))
    t[0, :] = teal(2)
    t[1, :] = teal(13)
    t[2:6, :] = teal(9)
    t[6, :] = teal(5)
    t[7, :] = teal(2)
    for x in (2, 6):
        t[1:7, x] = teal(5)
    t[3:5, 4] = red(6)            # a lashing
    if not left:
        t[:, 0] = teal(2)
    if not right:
        t[:, T - 1] = teal(2)
    return t


def vine_bridge(rng, variant):
    t = tile()
    t[0, :] = teal(2)
    t[1, :] = red(10)
    t[2, :] = red(8)
    t[3, :] = teal(2)
    for y in range(4, T):
        if (y + variant) % 2 == 0:
            t[y, 2 if variant else 5] = teal(6)
    return t


def thorns(direction):
    t = tile()
    for x0 in (0, 3, 6):
        for y in range(3, T):
            half = (y - 3) // 2
            for dx in range(-half, half + 1):
                x = x0 + 1 + dx
                if 0 <= x < T:
                    t[y, x] = teal(4 if dx else 7)
        t[2, x0 + 1] = red(12)
    t[T - 1, :] = teal(3)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def carved(rng, v):
    """Carved temple blocks for back walls, dimmer than the foreground."""
    t = tile()
    for y in range(T):
        for x in range(T):
            t[y, x] = red(4 if (x + y + v) % 7 else 3)
    if v % 2:
        t[2:6, 2] = red(2)
        t[2, 2:6] = red(2)
    return t


def leaf_clump(w, h, rng):
    s = pix.new_image(w, h)
    for _ in range(w * h // 18):
        cx, cy = rng.rand(w), rng.rand(h)
        r = 2 + rng.rand(4)
        for y in range(max(0, cy - r), min(h, cy + r + 1)):
            for x in range(max(0, cx - r), min(w, cx + r + 1)):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    s[y, x] = teal(5 + rng.rand(6))
    return pix.outline(s, teal(1))


def idol():
    """A stone idol head carved in the temple wall, 24x32."""
    s = pix.new_image(24, 32, red(5))
    s[0:2, :] = red(8)
    for y, x in ((10, 6), (10, 16)):
        s[y:y + 4, x:x + 3] = red(1)
        s[y + 1, x + 1] = teal(12)
    s[18:20, 9:15] = red(2)
    s[22:26, 7:17] = red(3)
    for x in range(8, 16, 2):
        s[22:26, x] = red(9)
    return pix.outline(s, red(1))


def palm_tree():
    w, h = 32, 64
    s = pix.new_image(w, h)
    for y in range(16, h):
        x = 16 + int(3 * math.sin(y / 12.0))
        s[y, x - 1:x + 2] = red(6 if (y // 3) % 2 else 5)
    for k in range(6):
        ang = -math.pi / 2 + (k - 2.5) * 0.55
        for r in range(2, 15):
            x = int(round(16 + r * math.cos(ang)))
            y = int(round(16 + r * math.sin(ang) + r * r / 30.0))
            for dy in (-1, 0, 1):
                if 0 <= x < w and 0 <= y + dy < h:
                    s[y + dy, x] = teal(7 if dy else 9)
    return s


def backdrop(width, height, rng):
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        for x in range(width):
            level = 13 - (y * 4) // height
            img[y, x] = pix.rgb(level, min(15, level + 1), max(0, level - 3))
    # distant ziggurat
    cx = width // 2
    for step in range(7):
        y0 = height - 120 + step * 12
        half = 30 + step * 10
        for y in range(y0, y0 + 12):
            for x in range(cx - half, cx + half):
                img[y, x] = red(7 - (1 if step % 2 else 0))
    # canopy layers
    for layer, (base, top) in enumerate(((6, height - 90), (4, height - 50))):
        for x in range(width):
            t = top + int(10 * math.sin(x / (13.0 + layer * 6)) + 6 * math.sin(x / 5.3 + layer))
            for y in range(max(0, t), height):
                img[y, x] = teal(base + ((x * 7 + y * 3) % 5 == 0))
    return img


STAMP_CHARS = {"I": "idol", "P": "palm", "Q": "leaves", "q": "leaves_small"}


def build(font):
    rng = pix.Rng(1931)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for v in range(2):
            tiles[f"brick_{mask}_{v}"] = edges(stone(rng, v), up, right, down, left)
        tiles[f"girder_{mask}"] = bamboo(up, right, down, left)
    tiles["plank_0"] = vine_bridge(rng, 0)
    tiles["plank_1"] = vine_bridge(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = thorns(d)
    for v in range(4):
        tiles[f"facade_{v}"] = carved(rng, v)
    stamps = {"idol": idol(), "palm": palm_tree(), "leaves": leaf_clump(48, 24, rng),
              "leaves_small": leaf_clump(24, 16, rng)}
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
