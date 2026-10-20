"""Region 1 art: "Safety Last Street", a silent-era slapstick city set.

Everything here is grayscale with a faint silver-sepia warmth (the silent
era); only the stunt double herself is in color. Readability rules for the
platforming: foreground solids are light with dark outlines, background
facades are darker and lower in contrast, and the painted backdrop is a hazy
light gray, so collidable surfaces always stand out.
"""
import numpy as np

import pix
from pix import TRANSPARENT, gray

T = 8  # tile size in pixels


def g(level):
    return gray(level, warm=1)


def tile(fill=TRANSPARENT):
    return np.full((T, T), fill, dtype=np.uint16)


# ---------------------------------------------------------------- foreground

def brick_fill(rng, base=9, mortar=4, rows_offset=0):
    """8x8 of running-bond brick: 4-pixel courses, mortar lines, speckle."""
    t = tile(g(base))
    for y in range(T):
        course = (y + rows_offset) // 4
        if (y + rows_offset) % 4 == 3:
            t[y, :] = g(mortar)
            continue
        joint = 0 if course % 2 == 0 else 4
        t[y, joint % T] = g(mortar)
        for x in range(T):
            if x == joint % T:
                continue
            n = rng.rand(9)
            if n == 0:
                t[y, x] = g(base + 1)
            elif n == 1:
                t[y, x] = g(base - 1)
    return t


def apply_edges(t, up, right, down, left, light=13, dark=1, shade=6):
    """Outline the sides of a solid tile that face air. `up` etc. are True
    when the neighbor in that direction is also solid."""
    t = t.copy()
    if not up:
        t[0, :] = g(dark)
        t[1, :] = g(light)
        t[2, :] = g(light - 2)
    if not down:
        t[T - 1, :] = g(dark)
        t[T - 2, :] = g(shade)
    if not left:
        t[:, 0] = g(dark)
        if up:
            t[1:, 1] = np.where(t[1:, 1] == g(dark), t[1:, 1], g(light - 3))
    if not right:
        t[:, T - 1] = g(dark)
        t[:, T - 2] = np.where(t[:, T - 2] == g(dark), t[:, T - 2], g(shade))
    if not up:
        t[0, :] = g(dark)
    return t


def girder(up, right, down, left):
    """Riveted steel I-beam, the secondary solid material."""
    t = tile(g(7))
    t[0, :] = g(1)
    t[1, :] = g(14)
    t[2, :] = g(11)
    t[3, :] = g(5)
    t[4, :] = g(4)
    t[5, :] = g(9)
    t[6, :] = g(7)
    t[7, :] = g(1)
    t[3, 2] = g(12)
    t[3, 6] = g(12)
    t[4, 2] = g(2)
    t[4, 6] = g(2)
    if not left:
        t[:, 0] = g(1)
    if not right:
        t[:, T - 1] = g(1)
    return t


def plank(rng, variant):
    """Scaffold plank (one-way): a 3-pixel board on the tile's top."""
    t = tile()
    t[0, :] = g(1)
    t[1, :] = g(12)
    t[2, :] = g(10)
    t[3, :] = g(6)
    t[4, :] = g(1)
    for x in range(T):
        if rng.rand(5) == 0:
            t[2, x] = g(8)
    if variant == 1:
        t[1:4, 0] = g(4)  # board seam
    return t


def nails(direction):
    """Upturned nails: the region's spike hazard."""
    t = tile()
    for i, x0 in enumerate((0, 4)):
        for y in range(4):
            half = y // 2
            for dx in range(-half, half + 1):
                x = x0 + 2 + dx
                if 0 <= x < T:
                    t[4 + y, x] = g(13 if dx < 0 else 9 if dx == 0 else 5)
        t[3, x0 + 2] = g(15)
        t[2, x0 + 2] = g(12)
    t[T - 1, :] = g(2)
    if direction == "down":
        t = t[::-1, :].copy()
    elif direction == "left":
        t = np.rot90(t, 1).copy()
    elif direction == "right":
        t = np.rot90(t, -1).copy()
    return t


# ---------------------------------------------------------------- background

def facade_fill(rng, variant):
    """Darker, low-contrast brick for the back walls of the set."""
    t = brick_fill(rng, base=5, mortar=3, rows_offset=variant * 2)
    return t


def window_stamp(lit, rng):
    """16x24 sash window with a lintel; lit windows glow with a silhouette."""
    w, h = 16, 24
    s = pix.new_image(w, h, g(4))
    s[0:3, :] = g(9)       # stone lintel
    s[2, :] = g(6)
    s[3:, 1:w - 1] = g(2)  # frame shadow
    glass = g(12) if lit else g(3)
    pane = g(14) if lit else g(4)
    for y in range(5, h - 3):
        for x in range(3, w - 3):
            s[y, x] = glass
    s[5:h - 3, w // 2] = g(7)          # mullion
    s[(5 + h - 3) // 2, 3:w - 3] = g(7)  # transom
    if lit:
        for y in range(6, 9):
            s[y, 4] = pane
        if rng.rand(2) == 0:  # a silhouette at the window
            cx = 5 + rng.rand(5)
            for y in range(10, h - 3):
                for x in range(cx, min(cx + 3, w - 3)):
                    s[y, x] = g(5)
            s[9, cx + 1] = g(5)
    s[h - 3:, :] = g(8)   # sill
    s[h - 1, :] = g(3)
    return s


def door_stamp():
    w, h = 16, 32
    s = pix.new_image(w, h, g(3))
    s[0:3, :] = g(9)
    s[3:, 2:w - 2] = g(5)
    s[5:h, 4:w - 4] = g(6)
    for y in (10, 20):
        s[y, 4:w - 4] = g(4)
    s[16, w - 6] = g(13)  # knob
    return s


def sign_stamp(text, font):
    """A painted shop sign: light letters on a dark board with a frame."""
    width = len(text) * 6 + 6
    s = pix.new_image(width, 11, g(2))
    s[0, :] = g(10)
    s[-1, :] = g(1)
    s[:, 0] = g(10)
    s[:, -1] = g(1)
    for i, ch in enumerate(text):
        glyph = font.get(ch)
        if glyph is None:
            continue
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit == "#":
                    s[3 + gy, 3 + i * 6 + gx] = g(14)
    return s


def lamp_stamp():
    """Cast-iron street lamp, 8x40, with a glowing globe."""
    w, h = 9, 40
    s = pix.new_image(w, h)
    for y in range(8, h):
        s[y, 4] = g(2)
        s[y, 3] = g(4) if y % 6 else g(2)
    s[h - 3:, 2:7] = g(2)
    for y in range(0, 7):
        for x in range(1, 8):
            dx, dy = x - 4, y - 3
            if dx * dx + dy * dy <= 9:
                s[y, x] = g(15 if dx * dx + dy * dy <= 2 else 13)
    s[7, 3:6] = g(2)
    return s


def on_ground(s):
    """Pad a prop with transparent rows on top to whole tiles, so a stamp
    placed by its top-left tile stands on the floor below it."""
    pad = -s.shape[0] % 8
    out = pix.new_image(s.shape[1], s.shape[0] + pad)
    out[pad:, :] = s
    return out


def poster_film_stamp():
    """A Silverlake Pictures one-sheet pasted on a wall, 20x28: a woman in
    an aviator cap against a spotlight, the title in tiny lines below."""
    s = pix.new_image(20, 28, g(2))
    s[1:27, 1:19] = g(12)
    for y in range(2, 18):
        for x in range(2, 18):
            d = (x - 10) ** 2 + (y - 7) ** 2
            s[y, x] = g(14) if d < 30 else g(10) if d < 70 else g(7)
    for y, x0, x1, lv in ((5, 8, 12, 3), (6, 7, 13, 3), (7, 7, 13, 4), (8, 8, 12, 5),
                          (9, 9, 11, 5), (10, 7, 13, 3), (11, 6, 14, 3), (12, 6, 14, 3),
                          (13, 6, 14, 3), (14, 7, 13, 3), (15, 7, 13, 3), (16, 7, 13, 3)):
        s[y, x0:x1] = g(lv)                     # the silhouette
    s[6, 11:13] = g(9)                          # goggles catching the light
    for y, x0, x1 in ((19, 4, 16), (21, 3, 17), (23, 6, 14), (25, 8, 12)):
        s[y, x0:x1] = g(4)                      # title and credits
    s[0, 3:6] = g(0)                            # a torn top corner
    return s


def poster_wanted_stamp():
    """A WANTED bill, 16x22: a sketched face and lines of small print."""
    s = pix.new_image(16, 22, g(3))
    s[1:21, 1:15] = g(13)
    s[2:4, 3:13] = g(4)                         # the WANTED headline
    for y in range(6, 14):
        for x in range(5, 11):
            if (x - 7.5) ** 2 / 9 + (y - 9.5) ** 2 / 16 <= 1:
                s[y, x] = g(9)
    s[9, 6] = g(2)
    s[9, 9] = g(2)
    s[12, 6:10] = g(5)
    s[6:8, 4:12] = g(4)                         # a bowler hat
    for y in (15, 17, 19):
        s[y, 3:13] = g(7)
    return s


def hydrant_stamp():
    """A fire hydrant, 8x12."""
    s = pix.new_image(8, 12)
    s[0, 3:5] = g(6)
    s[1:3, 2:6] = g(8)
    s[3:11, 2:6] = g(7)
    s[3:11, 2] = g(10)
    s[5:7, 0:8] = g(6)
    s[5:7, 0] = g(9)
    s[11, 1:7] = g(4)
    return pix.outline(s, g(1))


def trash_can_stamp():
    """A dented ash can with its lid, 10x14."""
    s = pix.new_image(10, 14)
    s[1:3, 0:10] = g(9)
    s[0, 4:6] = g(7)
    s[3:14, 1:9] = g(7)
    for y in (5, 9, 12):
        s[y, 1:9] = g(5)
    s[3:14, 1] = g(9)
    s[7, 6] = g(5)                              # a dent
    return pix.outline(s, g(1))


def mailbox_stamp():
    """A street mailbox on short legs, 10x15."""
    s = pix.new_image(10, 15)
    for y in range(0, 4):
        half = 2 + y
        s[y, 5 - half:5 + half] = g(6)
    s[4:12, 0:10] = g(6)
    s[4:12, 0] = g(9)
    s[5, 2:8] = g(2)                            # the slot
    s[8:10, 3:7] = g(10)                        # the plate
    s[12:15, 1] = g(3)
    s[12:15, 8] = g(3)
    return pix.outline(s, g(1))


def clock_stamp():
    """The big clock face (homage to Safety Last!), 40x40."""
    n = 40
    s = pix.new_image(n, n)
    c = (n - 1) / 2.0
    for y in range(n):
        for x in range(n):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if d <= 19.5:
                s[y, x] = g(2)
            if d <= 18:
                s[y, x] = g(9)
            if d <= 16.5:
                s[y, x] = g(14)
    for k in range(12):
        ang = k * np.pi / 6
        for r in (13, 14, 15):
            x = int(round(c + r * np.sin(ang)))
            y = int(round(c - r * np.cos(ang)))
            s[y, x] = g(2)
    for r in range(0, 11):  # hour hand near 10, minute hand near 2
        x = int(round(c + r * np.sin(-1.1)))
        y = int(round(c - r * np.cos(-1.1)))
        s[y, x] = g(1)
    for r in range(0, 15):
        x = int(round(c + r * np.sin(1.05)))
        y = int(round(c - r * np.cos(1.05)))
        s[y, x] = g(1)
    s[int(c) - 1:int(c) + 2, int(c) - 1:int(c) + 2] = g(1)
    return s


# ---------------------------------------------------------------- backdrop

def backdrop(width, height, rng):
    """Painted canvas backdrop: hazy sky, soft clouds, two skyline layers."""
    img = np.zeros((height, width), dtype=np.uint16)
    for y in range(height):
        level = 13 - (y * 3) // height
        for x in range(width):
            # ordered dither between two gray levels for a smooth gradient
            frac = ((y * 3 * 4) // height) % 4
            bayer = ((x & 1) * 2 + (y & 1) * 3) % 4
            img[y, x] = g(level - (1 if bayer < frac else 0))
    # clouds: soft ellipses
    for _ in range(9):
        cx = rng.rand(width)
        cy = 10 + rng.rand(height // 3)
        rx = 18 + rng.rand(30)
        ry = 5 + rng.rand(6)
        for y in range(max(0, cy - ry), min(height, cy + ry + 1)):
            for x in range(cx - rx, cx + rx + 1):
                xx = x % width
                d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
                if d <= 1.0:
                    img[y, xx] = g(15 if d < 0.55 or (d < 0.8 and (x + y) % 2) else 14)
    # far skyline
    x = 0
    while x < width:
        bw = 14 + rng.rand(26)
        bh = height // 3 + rng.rand(height // 4)
        for yy in range(height - bh, height):
            for xx in range(x, min(width, x + bw)):
                img[yy, xx] = g(10)
        for wy in range(height - bh + 4, height - 4, 6):
            for wx in range(x + 3, min(width, x + bw) - 2, 5):
                if rng.rand(3) == 0:
                    img[wy, wx] = g(12)
        if rng.rand(3) == 0:  # water tower
            tx = x + bw // 2
            for yy in range(height - bh - 10, height - bh):
                for xx in range(tx - 4, tx + 5):
                    if 0 <= xx < width:
                        img[yy, xx] = g(10)
        x += bw + rng.rand(4)
    # near skyline
    x = 0
    while x < width:
        bw = 20 + rng.rand(34)
        bh = height // 5 + rng.rand(height // 4)
        for yy in range(height - bh, height):
            for xx in range(x, min(width, x + bw)):
                img[yy, xx] = g(8)
        x += bw + 2 + rng.rand(10)
    # canvas seams of a painted backdrop
    for sx in range(0, width, 96):
        img[:, sx] = np.where(img[:, sx] > 0, img[:, sx], img[:, sx])
    return img


def build(font):
    """Return (tiles, stamps, backdrop) for region 1."""
    rng = pix.Rng(1923)
    tiles = {}
    tiles["air"] = tile()
    for mask in range(16):
        up, right, down, left = bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)
        for v in range(2):
            base = brick_fill(rng, base=10, mortar=5, rows_offset=v * 2)
            tiles[f"brick_{mask}_{v}"] = apply_edges(base, up, right, down, left)
        tiles[f"girder_{mask}"] = girder(up, right, down, left)
    tiles["plank_0"] = plank(rng, 0)
    tiles["plank_1"] = plank(rng, 1)
    for d in ("up", "down", "left", "right"):
        tiles[f"nails_{d}"] = nails(d)
    for v in range(4):
        tiles[f"facade_{v}"] = facade_fill(rng, v)
    stamps = {
        "window_lit": window_stamp(True, rng),
        "window_dark": window_stamp(False, rng),
        "window_lit2": window_stamp(True, rng),
        "door": door_stamp(),
        "lamp": lamp_stamp(),
        "clock": clock_stamp(),
        "sign_hotel": sign_stamp("HOTEL", font),
        "sign_bank": sign_stamp("BANK", font),
        "sign_cafe": sign_stamp("CAFE", font),
        "poster_film": poster_film_stamp(),
        "poster_wanted": poster_wanted_stamp(),
        "hydrant": on_ground(hydrant_stamp()),
        "trash_can": on_ground(trash_can_stamp()),
        "mailbox": on_ground(mailbox_stamp()),
    }
    back = backdrop(480, 236, rng)
    return tiles, stamps, back
