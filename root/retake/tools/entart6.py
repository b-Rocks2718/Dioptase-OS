"""Stage 6 actors ("Rocket Rangers of the Moon") and the Robot Emperor.

Drawn with shape helpers rather than ASCII grids: tin robots, moon frogs,
flying saucers (main.c hangs them from visible wires), ray-gun Martians,
rocket sleds, ray blasts, the hover pad, and the boss. Like every enemy
sheet, the art faces left; the renderer mirrors it for rightward motion.
Sizes match the Stage 5 sheet so hitbox alignment stays familiar. The
robots and Martians are dressed on cast.py's hand-drawn walk.
"""
import numpy as np

import pix

rgb = pix.rgb
OUT = rgb(2, 0, 3)
SILVER = rgb(12, 12, 13)
STEEL = rgb(8, 8, 10)
DARK = rgb(5, 5, 7)
WHITE = rgb(15, 15, 15)
RED = rgb(14, 2, 2)
YELLOW = rgb(15, 14, 3)
CYAN = rgb(6, 15, 15)
TEAL = rgb(2, 10, 10)
GREEN = rgb(4, 13, 5)
LIME = rgb(10, 15, 4)
MAGENTA = rgb(14, 3, 12)
PURPLE = rgb(8, 2, 10)
ORANGE = rgb(15, 8, 1)


def canvas(w, h):
    return pix.new_image(w, h)


def rect(img, x0, y0, x1, y1, c):
    img[y0:y1, x0:x1] = c


def ellipse(img, cx, cy, rx, ry, c, top_only=False):
    h, w = img.shape
    yy, xx = np.mgrid[0:h, 0:w]
    m = ((xx + 0.5 - cx) / rx) ** 2 + ((yy + 0.5 - cy) / ry) ** 2 <= 1
    if top_only:
        m &= yy + 0.5 <= cy
    img[m] = c


def done(img):
    return pix.outline(img, OUT)


def frog(leap):
    """A lime moon frog with eye stalks, 12x10."""
    s = canvas(12, 10)
    if leap:
        ellipse(s, 6, 4.5, 5, 2.5, LIME)
        rect(s, 2, 7, 4, 9, GREEN)
        rect(s, 8, 7, 10, 9, GREEN)
        eyes_y = 1
    else:
        ellipse(s, 6, 6.5, 5, 3, LIME)
        ellipse(s, 6, 7.5, 4, 1.5, GREEN)
        eyes_y = 2
    for x in (3, 8):
        s[eyes_y, x] = YELLOW
        s[eyes_y + 1, x] = GREEN
    s[eyes_y, 3] = OUT
    return done(s)


def saucer(blink):
    """A flying saucer, 14x8 (its wire is drawn by main.c)."""
    s = canvas(14, 8)
    ellipse(s, 7, 3.5, 3, 2.5, CYAN, top_only=True)
    ellipse(s, 7, 4.5, 6, 2, SILVER)
    rect(s, 2, 5, 12, 6, STEEL)
    for i, x in enumerate((3, 6, 9, 11)):
        s[5, x] = YELLOW if (i + blink) % 2 else RED
    return done(s)


def sled(flame):
    """A rocket sled, nose to the left, 18x12."""
    s = canvas(18, 12)
    ellipse(s, 8, 5.5, 7, 3.5, RED)
    rect(s, 4, 3, 12, 5, rgb(15, 6, 6))
    ellipse(s, 2.5, 5.5, 2, 2, SILVER)
    s[4, 7] = CYAN
    s[4, 9] = CYAN
    rect(s, 3, 9, 13, 10, DARK)
    for x in (4, 11):
        rect(s, x, 9, x + 2, 11, STEEL)
    n = 3 if flame else 2
    rect(s, 15, 4, 15 + n - 1, 7, ORANGE)
    s[5, 15] = YELLOW
    return done(s)


def ray_shot():
    s = canvas(6, 6)
    ellipse(s, 3, 3, 2.5, 2.5, MAGENTA)
    ellipse(s, 3, 3, 1.2, 1.2, WHITE)
    return s


def hover_pad():
    """The moving platform's look: a chrome pad with a teal underglow, 24x8."""
    s = canvas(24, 8)
    rect(s, 0, 0, 24, 1, OUT)
    rect(s, 0, 1, 24, 2, WHITE)
    rect(s, 0, 2, 24, 4, SILVER)
    rect(s, 0, 4, 24, 5, OUT)
    for x in range(2, 24, 5):
        rect(s, x, 5, x + 2, 6, CYAN)
        s[6, x] = TEAL
    return s


def emperor(pose):
    """The Robot Emperor, facing left, 48x56. pose: idle0, idle1, fire, jet."""
    s = canvas(48, 56)
    jet = pose == "jet"
    leg_top = 38 if not jet else 36
    leg_bot = 54 if not jet else 48
    for x0 in (14, 27):
        rect(s, x0, leg_top, x0 + 8, leg_bot, STEEL)
        rect(s, x0, leg_top, x0 + 2, leg_bot, SILVER)
        rect(s, x0 - 1, leg_bot - 3, x0 + 9, leg_bot, DARK)
    if jet:
        for x0 in (14, 27):
            rect(s, x0 + 1, 48, x0 + 7, 52, ORANGE)
            rect(s, x0 + 3, 52, x0 + 5, 55, YELLOW)
    rect(s, 9, 18, 40, 39, SILVER)
    rect(s, 9, 18, 12, 39, WHITE)
    rect(s, 36, 18, 40, 39, STEEL)
    ellipse(s, 24, 28, 6, 6, RED)               # the emblem: a ringed planet
    rect(s, 15, 27, 34, 29, YELLOW)
    rect(s, 13, 4, 35, 19, SILVER)
    rect(s, 13, 4, 15, 19, WHITE)
    rect(s, 16, 7, 32, 16, rgb(1, 6, 7))        # the television face
    blink = pose == "idle1"
    for ex in (19, 27):
        rect(s, ex, 9, ex + 3, 11 if blink else 12, YELLOW if pose != "fire" else RED)
    for x in range(18, 30):
        s[14 - (x % 2), x] = CYAN
    for i, x in enumerate(range(13, 36, 4)):    # the crown
        rect(s, x, 0 if i % 2 == 0 else 2, x + 2, 4, YELLOW)
    rect(s, 13, 3, 35, 4, ORANGE)
    if pose == "fire":
        rect(s, 0, 20, 10, 24, STEEL)            # arm thrust forward
        rect(s, 0, 21, 3, 23, YELLOW)
        ellipse(s, 2, 18, 2.5, 2.5, MAGENTA)
        s[18, 2] = WHITE
    else:
        rect(s, 4, 20, 9, 34, STEEL)
        rect(s, 4, 20, 5, 34, SILVER)
        rect(s, 2, 12 if pose != "idle1" else 13, 4, 36, YELLOW)    # the scepter
        ellipse(s, 3, 11 if pose != "idle1" else 12, 2.5, 2.5, MAGENTA)
    rect(s, 40, 20, 44, 34, STEEL)
    return done(s)


def build6():
    import cast
    humans = cast.stage_humanoids(6)
    return [
        ("walker6", humans["walker"]),
        ("hopper6", [frog(0), frog(1)]),
        ("flyer6", [saucer(0), saucer(1)]),
        ("thrower6", humans["thrower"]),
        ("charger6", [sled(0), sled(1)]),
        ("shot6", [ray_shot()]),
        ("pad6", [hover_pad()]),
        ("boss7", [emperor(p) for p in ("idle0", "idle1", "fire", "jet")]),
    ]
