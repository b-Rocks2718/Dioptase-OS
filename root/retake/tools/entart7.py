"""Stage 7 actors ("Night City") and the Fixer.

Noir black and white with neon touches: trench-coated thugs, alley cats,
blowing newspapers, bottle-throwing hoods, a bruiser who charges, thrown
bottles, tommy-gun bullets, and the boss. The art faces left, like every
enemy sheet. Sizes follow the Stage 5 and 6 sheets. The hoods are
dressed on cast.py's hand-drawn walk.
"""
import numpy as np

import pix

rgb = pix.rgb


def n(level):
    level = max(0, min(15, int(level)))
    return rgb(level, level, min(15, level + 1))


OUT = n(0)
PINK = rgb(15, 4, 11)
CYAN = rgb(4, 15, 15)
FLASH = rgb(15, 14, 8)


def canvas(w, h):
    return pix.new_image(w, h)


def rect(img, x0, y0, x1, y1, c):
    img[y0:y1, x0:x1] = c


def ellipse(img, cx, cy, rx, ry, c):
    h, w = img.shape
    yy, xx = np.mgrid[0:h, 0:w]
    img[((xx + 0.5 - cx) / rx) ** 2 + ((yy + 0.5 - cy) / ry) ** 2 <= 1] = c


def done(img):
    return pix.outline(img, OUT)


def cat(leap):
    """A black alley cat with glowing eyes, 12x10."""
    s = canvas(12, 10)
    if leap:
        ellipse(s, 6, 4.5, 5, 2, n(2))
        rect(s, 1, 2, 4, 5, n(2))
        rect(s, 9, 6, 11, 8, n(2))
        s[3, 2] = CYAN
    else:
        ellipse(s, 7, 6.5, 4, 2.5, n(2))
        rect(s, 2, 2, 5, 6, n(2))
        s[1, 2] = n(2)
        s[1, 4] = n(2)
        s[3, 3] = CYAN
        rect(s, 10, 3, 11, 7, n(2))
    return done(s)


def newspaper(flap):
    """A newspaper caught in the wind, 14x8."""
    s = canvas(14, 8)
    if flap:
        rect(s, 1, 2, 13, 6, n(13))
        rect(s, 6, 1, 8, 7, n(10))
    else:
        for x in range(1, 13):
            y0 = 1 + abs(x - 7) // 3
            rect(s, x, y0, x + 1, y0 + 4, n(13) if x < 7 else n(10))
    for x in range(3, 12, 2):
        s[3, x] = n(5)
    s[4, 4:10] = n(6)
    return done(s)


def bruiser(rush):
    """A big bruiser, head down and charging, 18x12."""
    s = canvas(18, 12)
    ellipse(s, 9, 6, 7, 4.5, n(5))
    ellipse(s, 9, 5, 6, 3.5, n(8))
    rect(s, 1, 3, 6, 8, n(11))           # bald head, low
    s[4, 2] = OUT
    rect(s, 3, 8, 6, 11, n(3))
    rect(s, 12, 8, 15, 11, n(3))
    if rush:
        for y in (3, 6, 9):
            rect(s, 15, y, 17, y + 1, n(9))   # speed lines
    return done(s)


def bottle():
    s = canvas(6, 6)
    rect(s, 2, 0, 4, 2, rgb(4, 9, 6))
    rect(s, 1, 2, 5, 6, rgb(3, 7, 5))
    s[3, 2] = rgb(9, 14, 10)
    return s


def bullet():
    s = canvas(6, 6)
    rect(s, 0, 2, 6, 4, FLASH)
    rect(s, 4, 2, 6, 4, rgb(15, 15, 15))
    return s


def fixer(pose):
    """The Fixer, facing left, 32x48. pose: duck, aim, fire, whistle."""
    s = canvas(32, 48)
    duck = pose == "duck"
    top = 18 if duck else 2
    rect(s, 7, top, 25, top + 3, n(2))                 # hat brim
    rect(s, 10, top - 2 if top >= 2 else 0, 22, top + 1, n(3))
    s[top, 10:22] = n(1)
    rect(s, 10, top + 1, 22, top + 2, PINK)            # the hatband, the one color he allows
    rect(s, 11, top + 3, 21, top + 10, n(12))          # face under the brim's shadow
    rect(s, 16, top + 3, 21, top + 10, n(7))
    rect(s, 11, top + 3, 21, top + 5, n(5))
    s[top + 6, 12] = OUT
    rect(s, 13, top + 8, 16, top + 9, n(4))            # a thin mustache
    body_top = top + 10
    rect(s, 6, body_top, 26, 42 if not duck else 44, n(6))   # double-breasted coat
    rect(s, 6, body_top, 9, 42 if not duck else 44, n(10))
    rect(s, 14, body_top, 18, body_top + 6, n(14))
    s[body_top + 1:body_top + 6, 16] = PINK
    if not duck:
        rect(s, 9, 42, 14, 47, n(2))
        rect(s, 18, 42, 23, 47, n(2))
    if pose in ("aim", "fire"):
        rect(s, 0, body_top + 4, 14, body_top + 8, n(3))     # the tommy gun
        ellipse(s, 9, body_top + 10, 3, 3, n(4))              # its drum
        rect(s, 12, body_top + 6, 17, body_top + 9, n(10))    # hands
        if pose == "fire":
            ellipse(s, 1.5, body_top + 6, 2, 3, FLASH)
            s[body_top + 6, 0] = rgb(15, 15, 15)
    elif pose == "whistle":
        rect(s, 9, top + 7, 12, top + 9, n(12))               # fingers to his lips
        rect(s, 7, top + 9, 11, body_top + 6, n(6))
    return done(s)


def build7():
    import cast
    humans = cast.stage_humanoids(7)
    return [
        ("walker7", humans["walker"]),
        ("hopper7", [cat(0), cat(1)]),
        ("flyer7", [newspaper(0), newspaper(1)]),
        ("thrower7", humans["thrower"]),
        ("charger7", [bruiser(0), bruiser(1)]),
        ("shot7", [bottle()]),
        ("bullet7", [bullet()]),
        ("boss8", [fixer(p) for p in ("duck", "aim", "fire", "whistle")]),
    ]
