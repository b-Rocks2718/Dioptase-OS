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
