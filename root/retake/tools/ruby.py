"""Ruby's sprite sheet, drawn by hand.

Ruby is a 1937 stunt double: leather aviator cap with brass goggles pushed
up, auburn bob, red scarf, flight jacket, jodhpurs and riding boots, and the
clapperboard she fights with. Each frame is a shared head (normal, blink or
wince) set on a per-pose body. Bodies are drawn on an 18-column grid whose
column 9 is the torso's center; the head's column 0 sits at the frame's `hx`.
A single outline pass finishes every frame. Frames face right; the engine
mirrors them for facing left.
"""
import numpy as np

import pix

rgb = pix.rgb

PALETTE = {
    "o": rgb(2, 1, 1),     # outline
    "e": rgb(1, 1, 2),     # eye
    "s": rgb(15, 11, 8),   # skin
    "S": rgb(12, 8, 6),    # skin shadow
    "m": rgb(11, 4, 4),    # mouth
    "h": rgb(10, 4, 2),    # auburn hair
    "c": rgb(7, 4, 2),     # leather cap
    "C": rgb(10, 6, 3),    # cap highlight
    "G": rgb(13, 10, 3),   # brass (goggle rims, belt buckle)
    "g": rgb(9, 14, 15),   # goggle glass
    "r": rgb(15, 3, 2),    # scarf
    "R": rgb(10, 1, 1),    # scarf shadow
    "q": rgb(15, 8, 6),    # scarf highlight
    "j": rgb(8, 6, 3),     # flight jacket
    "J": rgb(5, 4, 2),     # jacket shadow, far arm
    "k": rgb(11, 8, 4),    # sheepskin collar, near sleeve
    "p": rgb(13, 12, 9),   # jodhpurs
    "P": rgb(10, 9, 6),    # jodhpur shadow, far leg
    "b": rgb(4, 2, 1),     # boots and belt
    "B": rgb(7, 4, 2),     # boot shine
    "w": rgb(15, 15, 15),  # clapper stripe
    "x": rgb(1, 1, 1),     # clapper black
}

CANVAS = 32
FEET_Y = 30       # canvas row just below the soles (main.c CANVAS_BODY_Y = FEET_Y - 12)
CENTER_X = 16     # canvas column of the torso's center (main.c CANVAS_BODY_X = CENTER_X - 4)
BODY_CENTER = 9   # torso center column in the body grids below

HEAD = [
    ".....ccccc....",
    "...ccCCCCcGGG.",
    "..cGGGGGGGGgwG",
    ".hcccchhhsGGG.",
    ".hcccsssssesss",
    ".hcccsssssess.",
    ".hhhcSssssssS.",
    "..hh.SSssmsS..",
    "......SSSS....",
]


def _head(kind):
    rows = [list(r) for r in HEAD]
    if kind in ("blink", "wince"):
        rows[4][10] = "s"
        rows[5][10] = "e"
        rows[5][9] = "e"
    if kind == "wince":
        rows[7][9] = "e"            # a gasp
        rows[3][9] = "h"
    return ["".join(r) for r in rows]


# The clapperboard: sticks lifted open, or shut on the slate.
CLAP_OPEN = [
    "xw...",
    "wxw..",
    "..xwx",
    "wxwxw",
    "xxxxx",
    "xwwwx",
    "xxxxx",
]
CLAP_SHUT = [
    "wxwxw",
    "xxxxx",
    "xwwwx",
    "xxxxx",
]

BODIES = {}

BODIES["idle0"] = [
    ".......rrrrrq.....",
    "....rrRkkkkrrr....",
    "...rR.jjkjjjRr....",
    "..rR..jjjkjJjR....",
    "..R...JjjkjJjj....",
    "......JjjjjJss....",
    "......GbbbGbwxwx..",
    "......pppppPxxxx..",
    ".....ppppppPxwwx..",
    ".....pppP.PPxxxx..",
    "......bbB..bbB....",
    ".....bbbbB.bbbB...",
    ".....bbbbB.bbbbB..",
]
_SCARF_UP = [
    "...rrrRkkkkrrr....",
    "..rR..jjkjjjRr....",
    ".rR...jjjkjJjR....",
    "......JjjkjJjj....",
]
BODIES["idle1"] = BODIES["idle0"][:4] + BODIES["idle0"][5:]
BODIES["idle2"] = BODIES["idle0"][:1] + _SCARF_UP + BODIES["idle0"][5:]
BODIES["idle3"] = BODIES["idle0"][:1] + _SCARF_UP[:3] + BODIES["idle0"][5:]

# Running: contact, down, passing, then the same with the legs swapped. The
# clapper rides in the back of her belt; the arms swing against the legs.
BODIES["run0"] = [
    "........rrrrrq....",
    "..rrrrrRkkkkrrr...",
    "rrR....jjkjjjjRs..",
    "R.....kjjjkjjjJ...",
    ".....kkjjjkjjjj...",
    "....ss.jjjjjjjj...",
    "......xGbbbGbbb...",
    "......wPPpppppp...",
    ".....PPPP..pppp...",
    "....PPP.....ppp...",
    "...PPB.......pbb..",
    "..bbB........bbbB.",
    "..bB.........bbbbB",
]
BODIES["run1"] = [
    "........rrrrrq....",
    "..rrrrrRkkkkrrr...",
    "rrR....jjkjjjjRs..",
    "R.....kjjjkjjjJ...",
    ".....ksjjjkjjjj...",
    "......xjjjjjjjj...",
    "......wGbbbGbbb...",
    "....PPPPPpppppp...",
    "..bPPP.....ppp....",
    ".bbB.......pppp...",
    "...........bbbbB..",
    "...........bbbbbB.",
]
BODIES["run2"] = [
    "........rrrrrq....",
    "....rrrRkkkkrrr...",
    "..rrR..jjkjjjjR...",
    ".rR....jjjkjjjJ...",
    ".R.....jjjkkjjj...",
    "......xjjjjkkjj...",
    "......wGbbbGsbb...",
    "......xPPPppppp...",
    "......PPPPpppp....",
    ".....bBPP.ppp.....",
    ".....bB...ppp.....",
    "..........bbbB....",
    "..........bbbbB...",
]
BODIES["run3"] = [
    "........rrrrrq....",
    "...rrrrRkkkkrrr...",
    ".rrR...jjkjjjjR...",
    "rR.....jjjkjjjkk..",
    ".......jjjkjjjjss.",
    "......xjjjjjjjj...",
    "......wGbbbGbbb...",
    "......xpppppPPP...",
    ".....pppp...PPPP..",
    "....ppp......PPP..",
    "...ppb........PbB.",
    "..bbB.........bbbB",
    "..bB..........bbbB",
]
BODIES["run4"] = [
    "........rrrrrq....",
    "...rrrrRkkkkrrr...",
    ".rrR...jjkjjjjR...",
    "rR.....jjjkjjjkk..",
    ".......jjjkjjjjss.",
    "......xjjjjjjjj...",
    "......wGbbbGbbb...",
    "....ppppPPPPPP....",
    "..bppp.....PPP....",
    ".bbB.......PPPP...",
    "...........bbbbB..",
    "...........bbbbbB.",
]
BODIES["run5"] = [
    "........rrrrrq....",
    "....rrrRkkkkrrr...",
    "..rrR..jjkjjjjR...",
    ".rR....jjjkjjjJ...",
    ".R.....jjkkjjjj...",
    "......xjkkjjjjj...",
    "......wsbbbGbbb...",
    "......xpppPPPPP...",
    "......ppppPPPP....",
    ".....bBpp.PPP.....",
    ".....bB...PPP.....",
    "..........bbbB....",
    "..........bbbbB...",
]

BODIES["jump"] = [
    "........rrrrrq.ss.",
    "......rRkkkkrrkk..",
    ".....RjjkjjjjkR...",
    "....rRjjjkjjjj....",
    "...rR.jjjkjjjj....",
    "..rR..xjjjjjjj....",
    "..R...wGbbbGbb....",
    "......xPPppppppp..",
    ".......PPPpppppp..",
    "........PPP..bbbB.",
    "........PPP..bbbB.",
    "........bbbB......",
    "........bbbbB.....",
]
BODIES["fall"] = [
    "..rR....rrrrrq....",
    "s..rRrrRkkkkrrr..s",
    "JJ....jjkjjjjRr.kk",
    ".JJ...jjjkjjjj.kk.",
    "..JJ..jjjkjjjjkk..",
    "......xjjjjjjj....",
    "......wGbbbGbb....",
    "......xpppPPPP....",
    "......pppP.PPP....",
    ".....ppp...PPP....",
    ".....pbb....PbB...",
    "....bbbB....bbbB..",
    "....bbB.....bbbB..",
]
BODIES["fall1"] = [
    "...rR...rrrrrq....",
    "..rR.rrRkkkkrrr...",
    "s.....jjkjjjjRr..s",
    "JJJ...jjjkjjjj.kkk",
    "..JJJ.jjjkjjjjkk..",
] + BODIES["fall"][5:]
BODIES["land"] = [
    ".......rrrrrq.....",
    "...rrrRkkkkrrr....",
    "..rR.JjjkjjjRJ....",
    ".rR.JJjjjkjjjJk...",
    "...ss.JjjjjjjJss..",
    "......xGbbbGbb....",
    "....pppppPPPPPP...",
    "...ppp......PPP...",
    "..bbbB......bbbB..",
    "..bbbbB.....bbbbB.",
]

# Clapping: the slate comes up with its sticks open, then snaps shut at arm's
# length. The clapperboard itself is stamped at the hand (see CLAPPER below).
_CLAP_LEGS = [
    "......GbbbGbbb....",
    "......pppppPPP....",
    ".....ppppp.PPPP...",
    "....pppP....PPP...",
    "....bbB......bbB..",
    "...bbbbB.....bbbB.",
    "...bbbbB.....bbbbB",
]
BODIES["clap0"] = [
    ".......rrrrrq.....",
    "....rrRkkkkrrkk...",
    "...rR.jjkjjjRrkss.",
    "..rR..jjjkjjjj....",
    "..R...JjjkjjjJ....",
    "......JjjjjjjJ....",
] + _CLAP_LEGS
BODIES["clap1"] = [
    ".......rrrrrq.....",
    "....rrRkkkkrrr....",
    "...rR.jjkjjjkkkkss",
    "..rR..jjjkjjjj....",
    "..R...JjjkjjjJ....",
    "......JjjjjjjJ....",
] + _CLAP_LEGS

# Facing the wall she slides down: one hand and one boot braced on it.
BODIES["wall"] = [
    ".......rrrrrq.ss..",
    ".rr.rrRkkkkrrkk...",
    "rR..R.jjkjjjkk....",
    "R..R..jjjkjjjj....",
    "......JjjkjjjJ....",
    "......xjjjjjjJss..",
    "......wGbbbGbb....",
    "......xpppPPPPPP..",
    "......ppppPP.bbbB.",
    "......pppp...bbbB.",
    "......ppp.........",
    "......bbbB........",
    "......bbbbB.......",
]
BODIES["hurt"] = [
    "....ss.rrrrrq.ss..",
    ".....kkRkkkkrkk...",
    ".......kjkjjjj....",
    "......jjjkjjjj....",
    "......JjjkjjjJ....",
    "......JjjjjjjJ....",
    "......GbbbGbbb....",
    ".......pppPPPP....",
    "........ppPPPP....",
    "........pppPPPP...",
    ".........bbb.bbB..",
    "........bbbbBbbbB.",
    "........bbbbBbbbbB",
]
# Gliding under the prop umbrella: knees drawn up, near hand on the shaft.
BODIES["glide"] = [
    ".......rrrrrq.kk..",
    "..rrrrRkkkkrrkk...",
    "rrR...jjkjjjRr....",
    "R.....jjjkjjjj....",
    "......JjjkjjjJ....",
    "......xjjjjjjJ....",
    "......wGbbbGbb....",
    "......pppppPPP....",
    "......ppp.PPPP....",
    "......pbb..PPP....",
    ".....bbbB..PbB....",
    ".....bbB...bbbB...",
]

# Per frame: head kind, head column in the body grid, and an optional prop.
LAYOUT = {
    "idle0": ("normal", 2), "idle1": ("normal", 2), "idle2": ("blink", 2), "idle3": ("normal", 2),
    "run0": ("normal", 3), "run1": ("normal", 3), "run2": ("normal", 3),
    "run3": ("normal", 3), "run4": ("normal", 3), "run5": ("normal", 3),
    "jump": ("normal", 2), "fall": ("normal", 2), "fall1": ("normal", 2), "land": ("blink", 2),
    "clap0": ("normal", 1), "clap1": ("normal", 2), "wall": ("normal", 2),
    "hurt": ("wince", 1), "glide": ("normal", 1),
}

ORDER = ["idle0", "idle1", "idle2", "idle3", "run0", "run1", "run2", "run3", "run4", "run5",
         "jump", "fall", "fall1", "land", "clap0", "clap1", "wall", "hurt", "glide"]


def _stamp(img, rows, x0, y0):
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != "." and 0 <= y0 + y < CANVAS and 0 <= x0 + x < CANVAS:
                img[y0 + y, x0 + x] = PALETTE[ch]


def _umbrella(img, cx, top):
    """A striped prop umbrella: canopy 21 px wide with a shaft down to the hand."""
    for dx in range(-10, 11):
        depth = 5 - (abs(dx) * abs(dx)) // 22
        for y in range(top + 5 - depth, top + 6):
            img[y, cx + dx] = PALETTE["x"] if ((dx + 10) // 4) % 2 else PALETTE["r"]
        if abs(dx) % 4 == 2:
            img[top + 6, cx + dx] = PALETTE["x"]
    img[top - 1, cx] = PALETTE["x"]
    for y in range(top + 6, top + 12):
        img[y, cx] = PALETTE["x"]


def frame(name):
    """Compose one 32x32 frame: body, head on top, then the props."""
    head_kind, hx = LAYOUT[name]
    body = BODIES[name]
    img = pix.new_image(CANVAS, CANVAS)
    bx = CENTER_X - BODY_CENTER
    by = FEET_Y - len(body)
    _stamp(img, body, bx, by)
    _stamp(img, _head(head_kind), bx + hx, by - len(HEAD))
    if name in ("idle0", "idle1", "idle2", "idle3"):
        pass                                     # the slate is drawn into the body
    elif name == "clap0":
        _stamp(img, CLAP_OPEN, bx + 14, by - 5)
    elif name == "clap1":
        _stamp(img, CLAP_SHUT, bx + 18, by + 1)
        for k in range(3):                       # the crack of the sticks
            img[by - 1 - k, bx + 20 + k] = PALETTE["w"]
    elif name == "glide":
        _umbrella(img, bx + 14, 1)
    return pix.outline(img, PALETTE["o"])


def build_frames():
    return [frame(name) for name in ORDER]
