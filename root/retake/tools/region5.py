"""Region 5 art: "The Monster of Hollow Manor", German Expressionist horror.

Stark black-and-white with a cold violet cast, and sets painted the way the
Caligari designers painted them: light and shadow are painted onto the
scenery in jagged slashes, nothing is quite square, and the sky is a
painted spiral. Ground is slanted flagstone, girders are crooked beams,
planks are rickety boards, the hazard is a row of iron spikes, and back
walls carry painted zigzag shadows. Tile names match region1.py.
"""
import math

import numpy as np

import pix
from pix import TRANSPARENT

T = 8


def v(level):
    """Stark monochrome with a slight violet cast."""
    level = max(0, min(15, int(level)))
    return pix.rgb(level, max(0, level - 1), min(15, level + 1))


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def flagstone(rng, var):
    t = tile()
    for y in range(T):
        for x in range(T):
            # slanted bands: the expressionist tilt
            band = ((x + y * 2 + var * 3) // 4) % 3
            t[y, x] = v((11, 9, 10)[band] + (1 if rng.rand(12) == 0 else 0))
    for k in range(T):
        x = (k * 2 + var * 3) % T
        t[k, x] = v(3)
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = v(0)
        t[1, :] = v(15)
        t[2, :] = v(13)
    if not down:
        t[T - 1, :] = v(0)
        t[T - 2, :] = v(5)
    if not left:
        t[:, 0] = v(0)
    if not right:
        t[:, T - 1] = v(0)
    if not up:
        t[0, :] = v(0)
    return t


def beam(up, right, down, left):
    t = tile(v(6))
    t[0, :] = v(0)
    for x in range(T):
        t[1 + (x % 3 == 0), x] = v(13)   # a crooked highlight
    t[3:6, :] = v(5)
    for x in range(0, T, 3):
        t[4, x] = v(2)
    t[6, :] = v(3)
    t[7, :] = v(0)
    if not left:
        t[:, 0] = v(0)
    if not right:
        t[:, T - 1] = v(0)
    return t


def board(rng, variant):
    t = tile()
    t[0, :] = v(0)
    for x in range(T):
        sag = 1 if 2 <= x <= 5 else 0
        t[1 + sag, x] = v(12)
        t[2 + sag, x] = v(8)
        t[3 + sag, x] = v(0)
    if variant:
        t[1:4, 3] = v(2)
    return t


def spikes(direction):
    t = tile()
    for x0 in (0, 4):
        for y in range(1, T):
            half = (y - 1) // 3
            for dx in range(-half, half + 1):
                x = x0 + 2 + dx
                if 0 <= x < T:
                    t[y, x] = v(14 if dx < 0 else 7)
    t[T - 1, :] = v(2)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def painted_wall(rng, var):
    """Back walls with painted zigzag shadows."""
    t = tile()
    for y in range(T):
        for x in range(T):
            zig = (x + (y if (y // 4) % 2 == 0 else 7 - y) + var * 2) % 8
            t[y, x] = v(4 if zig < 3 else 2)
    return t


def portrait():
    s = pix.new_image(20, 26, v(2))
    s[0:2, :] = v(9)
    s[-2:, :] = v(9)
    s[:, 0:2] = v(9)
    s[:, -2:] = v(9)
    for y in range(6, 16):
        for x in range(6, 14):
            if ((x - 9.5) / 3.5) ** 2 + ((y - 10) / 4.5) ** 2 <= 1:
                s[y, x] = v(10)
    s[9, 8] = v(0)
    s[9, 11] = v(0)
    for y in range(16, 24):
        half = (y - 14) // 2 + 2
        s[y, 10 - half:10 + half] = v(5)
    return s


def tombstone():
    s = pix.new_image(16, 20)
    for y in range(20):
        for x in range(16):
            if y >= 6 or (x - 7.5) ** 2 + (y - 7) ** 2 <= 49:
                s[y, x] = v(8 if x < 9 else 6)
    s[6:13, 7:9] = v(3)
    s[8:10, 4:12] = v(3)
    return pix.outline(s, v(0))


def dead_tree():
    w, h = 32, 56
    s = pix.new_image(w, h)
    for y in range(20, h):
        x = 16 + (y - 20) // 9
        s[y, x - 1:x + 2] = v(1)
    branches = [(16, 20, 2, 4), (16, 26, 28, 12), (17, 32, 6, 18), (17, 24, 24, 2)]
    for x0, y0, x1, y1 in branches:
        n = max(abs(x1 - x0), abs(y1 - y0))
        for i in range(n + 1):
            x = x0 + (x1 - x0) * i // n
            y = y0 + (y1 - y0) * i // n
            if 0 <= x < w and 0 <= y < h:
                s[y, x] = v(1)
    return s


def tesla():
    """A laboratory coil, 16x40."""
    s = pix.new_image(16, 40)
    for y in range(8, 40):
        s[y, 6:10] = v(5 if (y // 2) % 2 else 9)
    for y in range(0, 8):
        for x in range(16):
            if (x - 7.5) ** 2 + (y - 4) ** 2 <= 16:
                s[y, x] = v(12)
    s[34:40, 2:14] = v(3)
    return pix.outline(s, v(0))


def backdrop(width, height, rng):
    img = np.zeros((height, width), dtype=np.uint16)
    cx, cy = width // 2, 70
    for y in range(height):
        for x in range(width):
            ang = math.atan2(y - cy, x - cx)
            r = math.hypot(x - cx, y - cy)
            spiral = int((ang * 3 + r / 14.0) * 2) % 4
            img[y, x] = v((3, 4, 3, 2)[spiral])
    for y in range(height):
        for x in range(width):
            if (x - cx) ** 2 + (y - cy) ** 2 <= 300:
                img[y, x] = v(13 if (x + y) % 5 else 11)
    # the crooked manor on its hill
    base = height - 40
    for y in range(base - 110, height):
        for x in range(width // 2 - 50, width // 2 + 60):
            lean = (base - y) // 6
            if width // 2 - 50 + lean <= x <= width // 2 + 40 + lean // 2 and y >= base - 100:
                img[y, x] = v(1)
    for wy in range(base - 90, base - 10, 22):
        for wx in range(width // 2 - 40, width // 2 + 30, 18):
            for y in range(wy, wy + 8):
                img[y, wx + (base - y) // 6:wx + 5 + (base - y) // 6] = v(11)
    for x in range(width):
        top = base + int(8 * math.sin(x / 31.0))
        for y in range(top, height):
            img[y, x] = v(1)
    return img


def arch_tiles():
    """Wainscot, rail, frieze and pilaster in painted-flat expressionist style."""
    out = {}
    for var in range(2):
        t = tile(v(3))
        t[0, :] = v(1)
        t[7, :] = v(1)
        for k in range(T):
            x = (k + var * 4) % T
            t[k, x] = v(6)                       # a slashed highlight
        out[f"wain_{var}"] = t
    t = tile(v(3))
    t[0:2, :] = v(9)
    t[2, :] = v(1)
    out["wain_top"] = t
    t = tile(v(5))
    t[5:8, :] = v(1)
    for x in range(T):
        t[5 + (x % 3), x] = v(7)                 # jagged painted teeth
    out["frieze"] = t
    t = tile(v(4))
    t[:, 1:3] = v(8)
    t[:, 6:8] = v(1)
    for y in range(0, T, 4):
        t[y, 3:6] = v(2)
    out["pilaster"] = t
    return out


STAMP_CHARS = {"P": "portrait", "G": "tombstone", "R": "dead_tree", "Z": "tesla", "L": "lamp"}


def build(font):
    import region1
    rng = pix.Rng(1920)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for var in range(2):
            tiles[f"brick_{mask}_{var}"] = edges(flagstone(rng, var), up, right, down, left)
        tiles[f"girder_{mask}"] = beam(up, right, down, left)
    tiles["plank_0"] = board(rng, 0)
    tiles["plank_1"] = board(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = spikes(d)
    for var in range(4):
        tiles[f"facade_{var}"] = painted_wall(rng, var)
    tiles.update(arch_tiles())
    lamp = region1.lamp_stamp()
    m = lamp != TRANSPARENT
    lamp[m] = np.array([v(int((c >> 4) & 15)) for c in lamp[m]], dtype=np.uint16)
    stamps = {"portrait": portrait(), "tombstone": tombstone(), "dead_tree": dead_tree(),
              "tesla": tesla(), "lamp": lamp}
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
