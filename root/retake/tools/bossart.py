"""Boss art, drawn procedurally so each pose is a few parameters.

Sergeant Gearhart, the Clockwork Constable (Stage 1): a barrel-chested
wind-up policeman with a bobby helmet, a walrus mustache, brass buttons and
a big key in his back. Frames are 40x44, facing left (the game mirrors them
when he faces right).
"""
import math

import numpy as np

import pix

W, H = 40, 44


def g(level):
    return pix.gray(level, warm=1)


def brass(level):
    level = max(0, min(15, level))
    return pix.rgb(min(15, level + 1), level, max(0, level - 4))


class Canvas:
    def __init__(self):
        self.img = pix.new_image(W, H)

    def disc(self, cx, cy, r, color, ry=None):
        ry = ry if ry is not None else r
        for y in range(int(cy - ry - 1), int(cy + ry + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if 0 <= x < W and 0 <= y < H and ((x - cx) / r) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    self.img[y, x] = color

    def rect(self, x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(H, y1)):
            for x in range(max(0, x0), min(W, x1)):
                self.img[y, x] = color

    def line(self, x0, y0, x1, y1, color, width=2):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(n + 1):
            t = i / n
            x = x0 + (x1 - x0) * t
            y = y0 + (y1 - y0) * t
            for dy in range(width):
                for dx in range(width):
                    xx, yy = int(x + dx - width // 2), int(y + dy - width // 2)
                    if 0 <= xx < W and 0 <= yy < H:
                        self.img[yy, xx] = color


def sergeant(pose):
    """pose: idle0, idle1, raise, slam, charge, dazed, whistle."""
    c = Canvas()
    lean = {"charge": -3, "slam": -1}.get(pose, 0)
    bob = 1 if pose == "idle1" else 0
    # legs and boots
    for lx in (14, 23):
        c.rect(lx + lean // 2, 34, lx + 5 + lean // 2, 41, g(3))
        c.rect(lx - 1 + lean // 2, 40, lx + 7 + lean // 2, 44, g(1))
    # the wind-up key on his back (right side, since he faces left)
    kx, ky = 33 + lean, 22 + bob
    c.rect(kx - 1, ky - 1, kx + 3, ky + 2, brass(8))
    spin = {"dazed": 1, "charge": 2}.get(pose, 0)
    if spin == 1:
        c.rect(kx + 2, ky - 5, kx + 4, ky + 6, brass(12))
    else:
        c.disc(kx + 4, ky - 4, 2.5, brass(12))
        c.disc(kx + 4, ky + 4, 2.5, brass(12))
        c.rect(kx + 2, ky - 2, kx + 4, ky + 3, brass(10))
    # barrel body: dark tunic with brass buttons and a belt
    c.disc(20 + lean, 26 + bob, 11, g(2), ry=10)
    c.disc(20 + lean, 26 + bob, 10, g(5), ry=9)
    c.rect(10 + lean, 30 + bob, 31 + lean, 32 + bob, g(1))
    c.rect(19 + lean, 30 + bob, 22 + lean, 32 + bob, brass(11))
    for by in (20, 24, 28):
        c.disc(15 + lean, by + bob, 1.2, brass(13))
    # head, mustache, eye
    hx, hy = 15 + lean, 12 + bob
    c.disc(hx, hy, 6, g(2))
    c.disc(hx, hy, 5, g(12))
    c.rect(hx - 6, hy + 1, hx + 2, hy + 3, g(2))          # walrus mustache
    c.disc(hx - 5, hy + 2, 1.5, g(2))
    eye = g(1) if pose != "dazed" else g(8)
    c.rect(hx - 3, hy - 2, hx - 1, hy, eye)
    if pose == "dazed":
        for sx, sy in ((hx - 6, hy - 10), (hx + 2, hy - 12), (hx + 8, hy - 9)):
            c.disc(sx, sy, 1.3, g(15))
    # bobby helmet with a badge
    c.disc(hx + 1, hy - 6, 6.5, g(1), ry=7)
    c.disc(hx + 1, hy - 6, 5.5, g(4), ry=6)
    c.rect(hx - 6, hy - 3, hx + 8, hy - 1, g(1))
    c.disc(hx - 1, hy - 7, 1.6, brass(13))
    # arm and baton
    sx, sy = 13 + lean, 21 + bob
    if pose in ("raise", "whistle"):
        ex, ey = sx - 4, sy - 12
        bx, by = ex + 2, ey - 9
    elif pose == "slam":
        ex, ey = sx - 9, sy + 6
        bx, by = ex - 10, ey + 6
    elif pose == "charge":
        ex, ey = sx - 8, sy - 2
        bx, by = ex - 9, ey - 1
    else:
        ex, ey = sx - 5, sy + 7
        bx, by = ex - 2, ey + 8
    c.line(sx, sy, ex, ey, g(5), 3)
    c.disc(ex, ey, 2, g(12))
    if pose == "whistle":
        c.rect(hx - 9, hy, hx - 5, hy + 2, brass(13))      # whistle at the mouth
    else:
        c.line(ex, ey, bx, by, g(9), 3)
    if pose == "charge":                                    # steam puffs
        for px, py in ((36, 8), (38, 12), (34, 4)):
            c.disc(px, py, 2.5, g(14))
    img = pix.outline(c.img, g(0))
    return img


def wave():
    """The baton slam's shockwave rolling along the floor."""
    s = pix.new_image(8, 8)
    for y in range(8):
        for x in range(8):
            h = 7 - abs(x - 3.5) * 1.6
            if 7 - y <= h:
                s[y, x] = g(14 if (x + y) % 2 else 11)
    return s


BOSS_FRAMES = ["idle0", "idle1", "raise", "slam", "charge", "dazed", "whistle"]


def build():
    return [("boss1", [sergeant(p) for p in BOSS_FRAMES]), ("wave", [wave()])]


if __name__ == "__main__":
    import sys
    frames = build()[0][1]
    strip = np.concatenate(frames, axis=1)
    pix.write_png(sys.argv[1] if len(sys.argv) > 1 else "/tmp/retake-prev/boss.png", strip, scale=5,
                  background=(150, 140, 120))


# ---------------------------------------------------------------- the Iron Horse

LW, LH = 72, 44


def amber(level):
    level = max(0, min(15, int(level)))
    return pix.rgb(min(15, level + (2 if level > 2 else 0)), level, max(0, level - 4))


def locomotive(frame, firebox=False):
    """A 4-4-0 locomotive facing left. frame rotates the wheel spokes."""
    c = Canvas.__new__(Canvas)
    c.img = pix.new_image(LW, LH)

    def disc(cx, cy, r, col, ry=None):
        ry = ry if ry is not None else r
        for y in range(int(cy - ry - 1), int(cy + ry + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if 0 <= x < LW and 0 <= y < LH and ((x - cx) / r) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    c.img[y, x] = col

    def rect(x0, y0, x1, y1, col):
        c.img[max(0, y0):min(LH, y1), max(0, x0):min(LW, x1)] = col

    # cab and roof
    rect(52, 8, 70, 33, amber(5))
    rect(50, 5, 72, 8, amber(3))
    rect(56, 12, 66, 20, amber(12) if firebox else amber(2))   # cab window
    # boiler with bands
    rect(12, 15, 54, 31, amber(6))
    rect(12, 15, 54, 17, amber(9))
    for bx in (20, 30, 40):
        rect(bx, 15, bx + 2, 31, amber(4))
    # smokebox front with headlamp
    rect(8, 13, 14, 32, amber(3))
    disc(12, 11, 3, amber(14))
    # smokestack (flared) and steam dome
    rect(17, 4, 23, 15, amber(3))
    rect(15, 1, 25, 5, amber(2))
    disc(36, 14, 4, amber(7), ry=3)
    # firebox at the back of the boiler
    rect(46, 24, 53, 31, amber(15) if firebox else amber(4))
    # cowcatcher
    for i in range(10):
        rect(1 + i, 31 + i // 2, 10, 32 + i // 2, amber(4))
    for x in range(2, 10, 2):
        rect(x, 32, x + 1, 37, amber(2))
    # frame
    rect(8, 31, 70, 34, amber(2))
    # wheels with rotating spokes
    for wx, wy, r in ((14, 39, 4), (30, 37, 6), (44, 37, 6), (60, 39, 4)):
        disc(wx, wy, r, amber(2))
        disc(wx, wy, r - 1, amber(8))
        for k in range(4):
            ang = (k * math.pi / 4) + frame * math.pi / 6
            for rr in range(r - 1):
                x = int(round(wx + rr * math.cos(ang)))
                y = int(round(wy + rr * math.sin(ang)))
                if 0 <= x < LW and 0 <= y < LH:
                    c.img[y, x] = amber(3)
        disc(wx, wy, 1, amber(13))
    # coupling rod between the drive wheels, riding with the crank
    ang = frame * math.pi / 6
    ox = int(round(3 * math.cos(ang)))
    oy = int(round(3 * math.sin(ang)))
    for x in range(30 + ox, 45 + ox):
        if 0 <= x < LW:
            c.img[37 + oy, x] = amber(13)
    if firebox:
        for px, py in ((24, 2), (28, 0), (32, 3)):
            disc(px, py, 2.5, amber(15))
    return pix.outline(c.img, amber(0))


def coal():
    s = pix.new_image(6, 6)
    for y in range(6):
        for x in range(6):
            if (x - 2.5) ** 2 + (y - 2.5) ** 2 <= 6:
                s[y, x] = pix.rgb(15, 8, 1) if (x + y) % 3 else pix.rgb(15, 13, 4)
    return s


def build_boss2():
    frames = [locomotive(f) for f in range(3)] + [locomotive(0, firebox=True)]
    return [("boss2", frames), ("coal", [coal()])]


# ---------------------------------------------------------------- the Kraken

def blue(level):
    level = max(0, min(15, int(level)))
    return pix.rgb(max(0, level - 4), max(0, level - 2), min(15, level + 1))


def tentacle_up():
    """A tentacle rising from the deck: 16x64, tapering to a curled tip."""
    w, h = 16, 64
    img = pix.new_image(w, h)
    for y in range(h):
        t = y / h                       # 0 at the tip (top), 1 at the base
        half = 2 + int(5 * t)
        sway = int(2 * math.sin(y / 7.0))
        cx = 8 + sway
        for x in range(cx - half, cx + half):
            if 0 <= x < w:
                img[y, x] = blue(7 if x < cx else 5)
        if y % 6 == 3 and half > 3:     # a row of suckers on the inner edge
            for dx in (-half + 1,):
                x = cx + dx
                if 0 <= x < w:
                    img[y, x] = blue(12)
                    if x + 1 < w:
                        img[y, x + 1] = blue(10)
    return pix.outline(img, blue(1))


def tentacle_down():
    """The same tentacle slammed across the deck: 64x16, tip to the left."""
    up = tentacle_up()
    return np.rot90(up, 1).copy()


def bubbles():
    img = pix.new_image(16, 8)
    for cx, cy, r in ((3, 5, 2), (8, 4, 3), (13, 5, 2)):
        for y in range(8):
            for x in range(16):
                d = (x - cx) ** 2 + (y - cy) ** 2
                if r * r - 2 <= d <= r * r + 1:
                    img[y, x] = blue(13)
    return img


def build_boss3():
    return [("tentacle_up", [tentacle_up()]), ("tentacle_down", [tentacle_down()]),
            ("bubbles", [bubbles()])]


# ---------------------------------------------------------------- the Everlasting Camera

def camera(frame, lens_open):
    """A giant hand-cranked camera on a tripod, facing left. 64x56."""
    w, h = 64, 56
    img = pix.new_image(w, h)
    metal = lambda l: pix.rgb(max(0, l - 1), l, min(15, l + 1))
    brassc = lambda l: pix.rgb(min(15, l + 2), l, max(0, l - 4))

    def disc(cx, cy, r, col):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if 0 <= x < w and 0 <= y < h and (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    img[y, x] = col

    def line(x0, y0, x1, y1, col):
        n = max(abs(x1 - x0), abs(y1 - y0)) * 2 + 1
        for i in range(n + 1):
            x = int(round(x0 + (x1 - x0) * i / n))
            y = int(round(y0 + (y1 - y0) * i / n))
            if 0 <= x < w and 0 <= y < h:
                img[y, x] = col

    # tripod
    line(36, 40, 22, 55, metal(4))
    line(36, 40, 50, 55, metal(4))
    line(36, 40, 36, 55, metal(3))
    # film magazines (two reels on top), spokes turning with the frame
    for cx in (28, 44):
        disc(cx, 10, 9, metal(3))
        disc(cx, 10, 8, metal(6))
        for k in range(3):
            ang = k * 2 * math.pi / 3 + frame * math.pi / 5
            line(cx, 10, int(round(cx + 7 * math.cos(ang))), int(round(10 + 7 * math.sin(ang))), metal(2))
        disc(cx, 10, 1.5, brassc(12))
    # body
    for y in range(18, 41):
        for x in range(22, 54):
            img[y, x] = metal(5 if x < 50 else 3)
    for x in range(22, 54):
        img[18, x] = metal(9)
    img[24:26, 28:48] = brassc(10)                      # a brass nameplate
    # crank on the back
    ang = frame * math.pi / 3
    cx, cy = 56, 30
    ex = int(round(cx + 5 * math.cos(ang)))
    ey = int(round(cy + 5 * math.sin(ang)))
    line(cx, cy, ex, ey, brassc(11))
    disc(ex, ey, 1.5, brassc(13))
    # lens barrel and the lens (an iris that opens)
    for y in range(22, 38):
        for x in range(8, 22):
            img[y, x] = metal(4 if (x // 3) % 2 else 6)
    disc(8, 30, 8, metal(2))
    if lens_open:
        disc(8, 30, 6.5, pix.rgb(15, 15, 11))
        disc(8, 30, 4, pix.rgb(15, 12, 4))
        disc(6, 28, 1.5, pix.rgb(15, 15, 15))
    else:
        disc(8, 30, 6.5, metal(1))
        for k in range(6):
            ang = k * math.pi / 3
            line(8, 30, int(round(8 + 6 * math.cos(ang))), int(round(30 + 6 * math.sin(ang))), metal(4))
    return pix.outline(img, pix.rgb(0, 0, 0))


def film_frame():
    s = pix.new_image(8, 8, pix.rgb(13, 13, 12))
    s[2:6, 2:6] = pix.rgb(5, 5, 6)
    for y in (0, 2, 4, 6):
        s[y, 0] = pix.rgb(2, 2, 2)
        s[y, 7] = pix.rgb(2, 2, 2)
    return s


def build_boss4():
    frames = [camera(f, False) for f in range(2)] + [camera(f, True) for f in range(2)]
    return [("boss4", frames), ("film", [film_frame()])]


# ---------------------------------------------------------------- Gargantua

def ape(pose):
    """A giant ape facing left, 64x64. pose: idle0, idle1, pound, throw, leap."""
    w, h = 64, 64
    img = pix.new_image(w, h)
    fur = lambda l: pix.rgb(max(0, l), max(0, l * 2 // 3 - 1), max(0, l // 2 - 1))
    skin = lambda l: pix.rgb(min(15, l + 2), max(0, l), max(0, l - 2))

    def disc(cx, cy, rx, ry, col):
        for y in range(int(cy - ry - 1), int(cy + ry + 2)):
            for x in range(int(cx - rx - 1), int(cx + rx + 2)):
                if 0 <= x < w and 0 <= y < h and ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    img[y, x] = col

    bob = 1 if pose == "idle1" else 0
    crouch = 6 if pose == "leap" else 0
    # legs
    disc(28, 54 - crouch // 2, 8, 9, fur(4))
    disc(44, 54 - crouch // 2, 8, 9, fur(4))
    # body
    disc(36, 38 + bob + crouch // 2, 18, 15, fur(5))
    disc(32, 40 + bob + crouch // 2, 9, 9, skin(9))      # chest
    # head
    hx, hy = 24, 18 + bob + crouch
    disc(hx, hy, 11, 10, fur(5))
    disc(hx - 3, hy + 3, 7, 6, skin(10))                 # face
    img[hy - 1:hy + 1, hx - 7:hx - 4] = fur(1)            # eyes
    img[hy - 1:hy + 1, hx - 1:hx + 2] = fur(1)
    img[hy + 5, hx - 8:hx + 1] = fur(2)                   # mouth
    disc(hx + 1, hy - 7, 8, 3, fur(4))                    # brow ridge
    # arms
    if pose == "pound":
        disc(22, 14, 6, 8, fur(4))
        disc(46, 14, 6, 8, fur(4))
    elif pose == "throw":
        disc(50, 20, 6, 10, fur(4))
        disc(50, 8, 7, 7, pix.rgb(9, 6, 4))               # the boulder held high
        disc(18, 46, 6, 10, fur(4))
    elif pose == "leap":
        disc(16, 34, 6, 9, fur(4))
        disc(54, 34, 6, 9, fur(4))
    else:
        disc(18, 46 + bob, 6, 11, fur(4))
        disc(52, 46 + bob, 6, 11, fur(4))
    return pix.outline(img, fur(0))


def boulder():
    s = pix.new_image(12, 12)
    for y in range(12):
        for x in range(12):
            d = (x - 5.5) ** 2 + (y - 5.5) ** 2
            if d <= 30:
                s[y, x] = pix.rgb(9, 6, 4) if (x * 3 + y) % 7 else pix.rgb(6, 4, 3)
            if d <= 6 and x < 6 and y < 6:
                s[y, x] = pix.rgb(12, 9, 6)
    return pix.outline(s, pix.rgb(2, 1, 1))


def build_boss5():
    frames = [ape(p) for p in ("idle0", "idle1", "pound", "throw", "leap")]
    return [("boss5", frames), ("boulder", [boulder()])]


# ---------------------------------------------------------------- the Monster

def monster(pose):
    """The Monster of Hollow Manor, facing left, 40x56. pose: idle0, idle1,
    raise (calling the lightning), stomp."""
    w, h = 40, 56
    img = pix.new_image(w, h)
    vi = lambda l: pix.rgb(max(0, min(15, l)), max(0, l - 1), min(15, l + 1))

    def rect(x0, y0, x1, y1, col):
        img[max(0, y0):min(h, y1), max(0, x0):min(w, x1)] = col

    bob = 1 if pose == "idle1" else 0
    # boots and legs
    stomp = 4 if pose == "stomp" else 0
    rect(12, 44, 19, 52 - stomp, vi(3))
    rect(22, 44, 29, 52, vi(3))
    rect(10, 52 - stomp, 20, 56 - stomp, vi(1))
    rect(21, 52, 31, 56, vi(1))
    # coat
    rect(10, 24 + bob, 31, 46, vi(4))
    rect(10, 24 + bob, 13, 46, vi(6))
    rect(19, 26 + bob, 21, 44, vi(2))
    # head: flat top, scar, bolts
    rect(13, 6 + bob, 28, 23 + bob, vi(11))
    rect(12, 4 + bob, 29, 9 + bob, vi(1))          # flat black hair
    rect(15, 13 + bob, 18, 15 + bob, vi(1))        # eyes, sunken
    rect(22, 13 + bob, 25, 15 + bob, vi(1))
    rect(15, 19 + bob, 26, 20 + bob, vi(3))        # grim mouth
    for y in range(9 + bob, 13 + bob):
        img[y, 20] = vi(4)                         # stitches
    rect(10, 16 + bob, 13, 19 + bob, vi(13))       # neck bolts
    rect(28, 16 + bob, 31, 19 + bob, vi(13))
    # arms
    if pose == "raise":
        rect(6, 4, 11, 26, vi(4))
        rect(30, 4, 35, 26, vi(4))
        rect(5, 0, 12, 5, vi(11))
        rect(29, 0, 36, 5, vi(11))
    else:
        rect(5, 24 + bob, 10, 42, vi(4))
        rect(31, 24 + bob, 36, 42, vi(4))
        rect(4, 41, 11, 45, vi(11))
        rect(30, 41, 37, 45, vi(11))
    return pix.outline(img, vi(0))


def build_boss6():
    return [("boss6", [monster(p) for p in ("idle0", "idle1", "raise", "stomp")])]
