"""Stage 8 actors ("Starlight Revue") and the Maestro.

Chorus dancers in top hats, a hopping tap shoe, paper stars hung on wires,
trumpeters who blow notes, a rolling bass drum, the notes themselves, the
paper-moon platform, and the boss. Art faces left, like every enemy sheet.
"""
import numpy as np

import pix

rgb = pix.rgb
OUT = rgb(2, 0, 1)
BLACK = rgb(3, 2, 4)
WHITE = rgb(15, 15, 15)
GOLD = rgb(15, 12, 3)
RED = rgb(14, 2, 3)
SKIN = rgb(15, 11, 8)
PINK = rgb(15, 7, 11)


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


def dancer(kick):
    """A chorus dancer in top hat and tails, 12x16; frame 1 kicks."""
    s = canvas(12, 16)
    rect(s, 4, 0, 8, 3, BLACK)
    rect(s, 3, 3, 9, 4, BLACK)
    rect(s, 4, 4, 8, 7, SKIN)
    s[5, 4] = OUT
    rect(s, 3, 7, 9, 11, BLACK)
    rect(s, 5, 7, 7, 10, WHITE)
    s[8, 6] = RED
    rect(s, 9, 7, 11, 9, BLACK)
    s[6, 10] = GOLD                     # the cane's knob
    rect(s, 10, 7, 11, 13, GOLD)
    if kick:
        rect(s, 4, 11, 6, 15, BLACK)
        rect(s, 0, 10, 4, 12, BLACK)
    else:
        rect(s, 3, 11, 5, 15, BLACK)
        rect(s, 7, 11, 9, 15, BLACK)
    return done(s)


def tap_shoe(leap):
    """A black tap shoe with a white spat, hopping, 12x10."""
    s = canvas(12, 10)
    y = 2 if leap else 4
    rect(s, 1, y + 2, 11, y + 5, BLACK)
    rect(s, 6, y, 11, y + 3, WHITE)
    s[y + 5, 2:10] = rgb(10, 10, 11)
    s[y + 1, 7] = OUT
    return done(s)


def paper_star(spin):
    """A paper star on a wire (main.c draws the wire), 14x8."""
    s = canvas(14, 8)
    if spin:
        rect(s, 6, 0, 8, 8, GOLD)
        rect(s, 3, 3, 11, 5, GOLD)
    else:
        ellipse(s, 7, 4, 3, 3, GOLD)
        for x, y in ((7, 0), (7, 7), (2, 4), (12, 4)):
            s[y, x] = GOLD
    s[3, 6] = rgb(15, 15, 11)
    return done(s)


def trumpeter(blowing):
    """A bandsman with a gold trumpet, 12x16."""
    s = canvas(12, 16)
    rect(s, 3, 0, 9, 3, RED)            # a shako
    s[0, 6] = GOLD
    rect(s, 4, 3, 8, 7, SKIN)
    rect(s, 3, 7, 9, 12, RED)
    rect(s, 3, 9, 9, 10, GOLD)
    rect(s, 3, 12, 5, 15, BLACK)
    rect(s, 7, 12, 9, 15, BLACK)
    rect(s, 0, 5, 4, 6, GOLD)
    rect(s, 0, 4 if blowing else 5, 2, 7 if blowing else 6, GOLD)
    return done(s)


def bass_drum(roll):
    """A rolling bass drum, 18x12."""
    s = canvas(18, 12)
    ellipse(s, 9, 6, 8, 5.5, RED)
    ellipse(s, 9, 6, 6, 4, WHITE)
    for k in range(4):
        x = 3 + ((k * 4 + (2 if roll else 0)) % 13)
        rect(s, x, 1, x + 1, 11, GOLD)
    s[6, 9] = RED
    return done(s)


def note():
    """A quaver, 6x6."""
    s = canvas(6, 6)
    ellipse(s, 2, 4.5, 2, 1.5, BLACK)
    rect(s, 3, 0, 4, 5, BLACK)
    rect(s, 4, 0, 6, 2, BLACK)
    return s


def moon_pad():
    """The paper-moon platform's look, 24x8: a crescent lying on its back."""
    s = canvas(24, 8)
    rect(s, 0, 0, 24, 1, OUT)
    rect(s, 0, 1, 24, 3, rgb(15, 14, 9))
    for x in range(24):
        depth = 3 + (4 - abs(x - 12) // 3)
        if depth > 3:
            rect(s, x, 3, x + 1, min(8, depth), rgb(13, 11, 6))
    return s


def maestro(pose):
    """The Maestro, facing left, 40x56. pose: idle0, idle1, conduct, bow."""
    s = canvas(40, 56)
    bow = pose == "bow"
    hx, hy = (10, 18) if bow else (14, 3)
    ellipse(s, hx + 6, hy + 2, 8, 4, WHITE)              # a wild white mane
    rect(s, hx + 1, hy + 2, hx + 11, hy + 12, SKIN)
    s[hy + 6, hx + 3] = OUT
    rect(s, hx + 2, hy + 9, hx + 6, hy + 10, WHITE)       # mustache
    top = hy + 12
    rect(s, 10, top, 30, 46, BLACK)                       # tailcoat
    rect(s, 16, top, 22, top + 10, WHITE)                 # shirt front
    rect(s, 17, top, 21, top + 2, RED)                    # bow tie
    rect(s, 24, 40, 32, 50, BLACK)                        # coat tails
    rect(s, 12, 46, 17, 55, BLACK)
    rect(s, 22, 46, 27, 55, BLACK)
    if pose == "conduct":
        rect(s, 4, 6, 10, top + 4, BLACK)
        rect(s, 2, 2, 6, 7, WHITE)                        # gloved hand high
        rect(s, 0, 0, 2, 4, rgb(15, 15, 11))              # the baton
        rect(s, 30, 8, 36, top + 4, BLACK)
        rect(s, 34, 4, 38, 9, WHITE)
    elif bow:
        rect(s, 4, top + 6, 12, top + 9, BLACK)
        rect(s, 2, top + 6, 5, top + 10, WHITE)
    else:
        sway = 1 if pose == "idle1" else 0
        rect(s, 6, top + 2, 11, top + 12, BLACK)
        rect(s, 4, top + 10 - sway, 8, top + 14 - sway, WHITE)
        rect(s, 0, top + 6 - sway * 3, 5, top + 7 - sway * 3, rgb(15, 15, 11))
    return done(s)


def build8():
    return [
        ("walker8", [dancer(0), dancer(1)]),
        ("hopper8", [tap_shoe(0), tap_shoe(1)]),
        ("flyer8", [paper_star(0), paper_star(1)]),
        ("thrower8", [trumpeter(0), trumpeter(1)]),
        ("charger8", [bass_drum(0), bass_drum(1)]),
        ("shot8", [note()]),
        ("moon8", [moon_pad()]),
        ("boss9", [maestro(p) for p in ("idle0", "idle1", "conduct", "bow")]),
    ]
