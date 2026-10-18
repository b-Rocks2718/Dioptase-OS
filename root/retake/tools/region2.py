"""Region 2 art: "Dust & Iron", a Western shot on amber-tinted stock.

Silent Westerns were printed on amber-tinted film for daylight scenes, so
the whole stage is a warm amber monochrome: sandstone strata for the ground
and cliffs, timber trestles, boardwalk planks, cactus clumps as the hazard,
plank-sided storefronts, and a sunset desert of mesas behind it all. Tile
names match region1.py so rooms.compose works unchanged.
"""
import numpy as np

import pix
import region1
from pix import TRANSPARENT

T = 8


def a(level):
    """Amber stock: a luminance level tinted orange-gold."""
    level = max(0, min(15, int(level)))
    r = min(15, level + (2 if level > 2 else 0))
    g = level
    b = max(0, level - 4)
    return pix.rgb(r, g, b)


def amberize(img):
    """Re-tint a warm-gray stamp from region 1 into amber stock."""
    out = img.copy()
    m = img != TRANSPARENT
    lv = (img[m] >> 4) & 15
    out[m] = np.array([a(int(v)) for v in lv], dtype=np.uint16)
    return out


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


def sandstone(rng, y_offset):
    """Layered rock: horizontal strata with pebbles."""
    t = tile()
    for y in range(T):
        band = ((y + y_offset) // 3) % 3
        base = (10, 8, 9)[band]
        for x in range(T):
            n = rng.rand(9)
            t[y, x] = a(base + (1 if n == 0 else -1 if n == 1 else 0))
    return t


def edges(t, up, right, down, left):
    t = t.copy()
    if not up:
        t[0, :] = a(1)
        t[1, :] = a(13)
        t[2, :] = a(12)
    if not down:
        t[T - 1, :] = a(1)
        t[T - 2, :] = a(5)
    if not left:
        t[:, 0] = a(1)
    if not right:
        t[:, T - 1] = a(1)
        t[1:T - 1, T - 2] = np.where(t[1:T - 1, T - 2] == a(1), a(1), a(6))
    if not up:
        t[0, :] = a(1)
    return t


def trestle(up, right, down, left):
    """A timber beam with iron bolts (the stage's girders)."""
    t = tile(a(7))
    t[0, :] = a(1)
    t[1, :] = a(11)
    t[2, :] = a(9)
    t[3:6, :] = a(7)
    for x in (1, 5):
        t[4, x] = a(2)
    t[6, :] = a(5)
    t[7, :] = a(1)
    for x in range(0, T, 3):
        t[3, x] = a(8)
    if not left:
        t[:, 0] = a(1)
    if not right:
        t[:, T - 1] = a(1)
    return t


def board(rng, variant):
    t = tile()
    t[0, :] = a(1)
    t[1, :] = a(12)
    t[2, :] = a(10)
    t[3, :] = a(7)
    t[4, :] = a(1)
    for x in range(T):
        if rng.rand(4) == 0:
            t[2, x] = a(8)
    if variant:
        t[1:4, 0] = a(3)
        t[2, 3] = a(2)
    return t


def cactus(direction):
    """A clump of cactus pads: the region's spike hazard."""
    t = tile()
    for cx, h in ((2, 6), (5, 5)):
        for y in range(T - h, T):
            t[y, cx] = a(6)
            t[y, cx + 1] = a(4)
        for y in range(T - h, T, 2):
            if cx - 1 >= 0:
                t[y, cx - 1] = a(13)
            if cx + 2 < T:
                t[y, cx + 2] = a(13)
    t[T - 1, :] = a(3)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


def siding(rng, variant):
    """Vertical plank siding for storefronts (back walls)."""
    t = tile()
    for x in range(T):
        base = 5 if (x + variant) % 4 else 3
        for y in range(T):
            t[y, x] = a(base + (1 if rng.rand(11) == 0 else 0))
    if variant == 2:
        t[3, 1] = a(2)
    return t


def sign_board(text, font, width=None):
    w = width or len(text) * 6 + 8
    s = pix.new_image(w, 13, a(3))
    s[0, :] = a(1)
    s[-1, :] = a(1)
    s[:, 0] = a(1)
    s[:, -1] = a(1)
    s[1, 1:-1] = a(6)
    x0 = (w - len(text) * 6) // 2 + 1
    for i, ch in enumerate(text):
        g = font.get(ch)
        if not g:
            continue
        for gy, row in enumerate(g):
            for gx, bit in enumerate(row):
                if bit == "#":
                    s[3 + gy, x0 + i * 6 + gx] = a(14)
    return s


def batwing():
    """Saloon batwing doors in a dark doorway, 16x32."""
    s = pix.new_image(16, 32, a(1))
    s[0:2, :] = a(7)
    for y in range(8, 24):
        for x in range(1, 7):
            s[y, x] = a(9 if x % 2 else 8)
        for x in range(9, 15):
            s[y, x] = a(9 if x % 2 else 8)
    s[8, 1:7] = a(11)
    s[8, 9:15] = a(11)
    return s


def saguaro():
    w, h = 24, 48
    s = pix.new_image(w, h)
    for y in range(4, h):
        for x in range(9, 15):
            s[y, x] = a(5 if x < 12 else 4)
    for y in range(14, 30):
        for x in range(2, 6):
            s[y, x] = a(5)
    s[28:31, 2:12] = a(5)
    for y in range(10, 24):
        for x in range(18, 22):
            s[y, x] = a(4)
    s[22:25, 12:22] = a(4)
    for x in range(9, 15):
        s[3, x] = a(5) if 10 <= x <= 13 else TRANSPARENT
    for y in range(6, h, 4):
        s[y, 10] = a(8)
    return pix.outline(s, a(2))


def windmill():
    w, h = 40, 64
    s = pix.new_image(w, h)
    cx, cy = 20, 14
    for y in range(cy, h):
        spread = (y - cy) // 6
        s[y, cx - 2 - spread] = a(3)
        s[y, cx + 2 + spread] = a(3)
        if (y - cy) % 10 == 0:
            s[y, cx - 2 - spread:cx + 3 + spread] = a(3)
    for k in range(8):
        ang = k * np.pi / 4
        for r in range(2, 13):
            x = int(round(cx + r * np.cos(ang)))
            y = int(round(cy + r * np.sin(ang)))
            if 0 <= x < w and 0 <= y < h:
                s[y, x] = a(6)
                if r > 6 and 0 <= x + 1 < w:
                    s[y, x + 1] = a(5)
    s[cy - 1:cy + 2, cx - 1:cx + 2] = a(2)
    s[cy:cy + 3, cx + 12:cx + 18] = a(4)   # tail vane
    return s


def mine_frame():
    """Timber frame of a mine entrance, 32x40."""
    s = pix.new_image(32, 40, a(1))
    s[0:5, :] = a(6)
    s[1, :] = a(9)
    for x in (0, 1, 2, 3, 28, 29, 30, 31):
        s[:, x] = a(6 if x in (1, 29) else 5)
    for y in range(8, 40, 6):
        s[y, 6:26] = a(2)
    return s


def wanted():
    s = pix.new_image(12, 16, a(12))
    s[0, :] = a(6)
    s[-1, :] = a(6)
    s[:, 0] = a(6)
    s[:, -1] = a(6)
    s[2, 2:10] = a(3)
    for y in range(5, 11):
        for x in range(4, 8):
            s[y, x] = a(5)
    s[12, 2:10] = a(4)
    s[14, 3:9] = a(4)
    return s


def water_tower():
    w, h = 32, 56
    s = pix.new_image(w, h)
    for y in range(4, 26):
        for x in range(3, 29):
            s[y, x] = a(7 if (x // 3) % 2 else 6)
    s[3, 4:28] = a(9)
    for y in range(0, 4):
        s[y, 6 + y * 2:26 - y * 2] = a(5)
    for y in range(4, 26, 7):
        s[y, 3:29] = a(3)
    for y in range(26, h):
        for x in (5, 6, 25, 26):
            s[y, x] = a(3)
        if (y - 26) % 9 == 0:
            s[y, 5:27] = a(3)
    return s


def backdrop(width, height, rng):
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        level = 14 - (y * 6) // height
        frac = ((y * 6 * 4) // height) % 4
        for x in range(width):
            bayer = ((x & 1) * 2 + (y & 1) * 3) % 4
            img[y, x] = a(level - (1 if bayer < frac else 0))
    # a big low sun
    sx, sy = width * 2 // 3, height // 2
    for y in range(height):
        for x in range(width):
            d = (x - sx) ** 2 + (y - sy) ** 2
            if d <= 26 * 26:
                img[y, x] = a(15)
    # far mesas
    x = 0
    while x < width:
        mw = 40 + rng.rand(60)
        mh = height // 4 + rng.rand(height // 6)
        top = height - mh - 30
        for yy in range(top, height):
            slant = (yy - top) // 3
            for xx in range(x - slant, x + mw + slant):
                if 0 <= xx < width:
                    img[yy, xx] = a(9)
        x += mw + 30 + rng.rand(50)
    # near buttes
    x = 10
    while x < width:
        mw = 20 + rng.rand(30)
        mh = height // 6 + rng.rand(height // 8)
        top = height - mh
        for yy in range(top, height):
            for xx in range(x, min(width, x + mw)):
                img[yy, xx] = a(7)
        for xx in range(x, min(width, x + mw)):
            img[top, xx] = a(10)
        x += mw + 50 + rng.rand(60)
    # desert floor haze
    for yy in range(height - 18, height):
        for xx in range(width):
            if (xx + yy) % 3 == 0:
                img[yy, xx] = a(8)
    return img


STAMP_CHARS = {
    "W": "window_lit", "w": "window_dark", "D": "batwing", "L": "lamp", "H": "sign_saloon",
    "K": "sign_jail", "F": "sign_store", "C": "saguaro", "T": "water_tower", "M": "windmill",
    "P": "wanted", "N": "mine",
}


def build(font):
    rng = pix.Rng(1925)
    tiles = {"air": tile()}
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for v in range(2):
            tiles[f"brick_{mask}_{v}"] = edges(sandstone(rng, v * 2), up, right, down, left)
        tiles[f"girder_{mask}"] = trestle(up, right, down, left)
    tiles["plank_0"] = board(rng, 0)
    tiles["plank_1"] = board(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = cactus(d)
    for v in range(4):
        tiles[f"facade_{v}"] = siding(rng, v)
    stamps = {
        "window_lit": amberize(region1.window_stamp(True, rng)),
        "window_dark": amberize(region1.window_stamp(False, rng)),
        "batwing": batwing(),
        "lamp": amberize(region1.lamp_stamp()),
        "sign_saloon": sign_board("SALOON", font),
        "sign_jail": sign_board("JAIL", font),
        "sign_store": sign_board("GEN STORE", font),
        "saguaro": saguaro(),
        "water_tower": water_tower(),
        "windmill": windmill(),
        "wanted": wanted(),
        "mine": mine_frame(),
    }
    back = backdrop(480, 300, rng)
    return tiles, stamps, back
