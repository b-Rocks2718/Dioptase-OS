"""Hand-drawn enemy cast, shared across the pictures.

Humanoids are a costume head on a body drawn by hand in a few poses: a
four-frame walk (contact, passing, contact, passing), a stand, and a throw.
Bodies and heads are written facing right in material roles rather than
colors, so each picture dresses the same walk in its own stock: `dress`
maps roles to tone levels, and the region's tint turns levels into colors.
Every finished frame gets one outline pass and is mirrored to face left,
like every enemy sheet.

Roles: c/C/L coat (base, shadow, light), p/P trousers (base, shadow),
b boots and belt, G buckle, w buttons and whites, s/S skin, e eyes,
M mustache, h/H hat (base, light), T held prop, k far arm, A accent.
"""
import numpy as np

import pix

# Walk cycle bodies: 14 wide, 13 tall, torso centered on column 6.
WALK = [
    [
        "...ccccccc....",
        "..CcwcwccLL...",
        ".CCcccccccLL..",
        ".CCcwcwcccLs..",
        ".sCccccccc.T..",
        "..CbbbGbbb.T..",
        "..Ccccccccc...",
        "..CPPcppppp...",
        ".CPP....ppp...",
        ".PP......pp...",
        "PP.......ppp..",
        "bbb......bbbb.",
        "bb.......bbbbb",
    ],
    [
        "...ccccccc....",
        "..CcwcwccL....",
        "..CcccccccL...",
        "..CcwcwcccL...",
        "..Ccccccccs...",
        "..CbbbGbbbT...",
        "..CcccccccT...",
        "...PPPpppp....",
        "....PPppp.....",
        "...bbPppp.....",
        "...bb..ppp....",
        "......bbbb....",
        "......bbbbb...",
    ],
    [
        "...ccccccc....",
        "..CcwcwccL....",
        ".kCcccccccL...",
        ".kCcwcwcccL...",
        ".sCcccccccs...",
        "..CbbbGbbbT...",
        "..CcccccccT...",
        "..CppppPPPP...",
        ".ppp....PPP...",
        ".pp......PP...",
        "pp.......PPP..",
        "bbb......bbbb.",
        "bb.......bbbbb",
    ],
    [
        "...ccccccc....",
        "..CcwcwccL....",
        "..CcccccccL...",
        "..CcwcwcccL...",
        "..Ccccccccs...",
        "..CbbbGbbbT...",
        "..CcccccccT...",
        "...pppPPPP....",
        "....ppPPP.....",
        "...bbpPPP.....",
        "...bb..PPP....",
        "......bbbb....",
        "......bbbbb...",
    ],
]
# Standing with the prop held up at the chest, then the throw: the near arm
# snaps forward and the prop leaves the hand (the shot entity carries it).
HOLD = [
    "...ccccccc.ww.",
    "..CcwcwcLLTT..",
    "..CcccccLsTT..",
    "..CcwcwccL....",
    "..CcccccccL...",
    "..CbbbGbbbs...",
    "..Ccccccccc...",
    "..CPPPpppp....",
    "..PPP..ppp....",
    "..PP....pp....",
    "..PP....pp....",
    ".bbbb...bbbb..",
    ".bbbbb..bbbbb.",
]
THROW = [
    "...ccccccLLs..",
    "..CcwcwcLL....",
    "..CcccccL.....",
    "..CcwcwccL....",
    "..CccccccL....",
    "..CbbbGbbb....",
    "..Ccccccccc...",
    "..CPPPpppp....",
    ".PPP....ppp...",
    ".PP......pp...",
    "PP........pp..",
    "bbb.......bbbb",
    "bb........bbbb",
]

# Costume heads, 10 wide, facing right; `hx` places them over the body.
HEADS = {
    "cop": [                      # a Keystone cop: domed helmet, badge, walrus mustache
        "...hhh....",
        "..hHhhh...",
        "..hhwhh...",
        "..hhhhh...",
        ".hhhhhhhh.",
        "..Sssssss.",
        "..Sssesss.",
        "..SsMMMMMM",
        "...SsssS..",
    ],
    "chef": [                     # a pastry chef in a tall toque
        "..wwwww...",
        ".wwwwwww..",
        ".wwwwwww..",
        "..wwwww...",
        "..HHHHH...",
        "..Sssssss.",
        "..Sssesss.",
        "..SssssMs.",
        "...SsssS..",
    ],
}


def _image(rows, colors, width):
    art = pix.new_image(width, len(rows))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != ".":
                art[y, x] = colors[ch]
    return art


def humanoid(head, body, colors, hx=2):
    """One frame: the head over the body, outlined, mirrored to face left."""
    width = 18
    rows = [("." * hx + r) for r in HEADS[head]] + body
    art = _image([r.ljust(width - 2, ".") for r in rows], colors, width - 2)
    framed = pix.new_image(width, len(rows) + 2)
    pix.blit(framed, art, 1, 1)
    return pix.mirror(pix.outline(framed, colors["o"]))


def dress(tone, **levels):
    """Role -> color for one costume. `tone(level)` is the picture's tint;
    `levels` overrides the default level of any role."""
    base = {"o": 0, "c": 4, "C": 3, "L": 6, "p": 3, "P": 2, "b": 1, "G": 11, "w": 14,
            "s": 12, "S": 9, "e": 1, "M": 2, "h": 3, "H": 5, "T": 9, "k": 3, "A": 10}
    base.update(levels)
    return {role: tone(level) for role, level in base.items()}


def walker(head, colors):
    return [humanoid(head, body, colors) for body in WALK]


def thrower(head, colors):
    return [humanoid(head, HOLD, colors), humanoid(head, THROW, colors)]


# ---------------------------------------------------------------- critters

def _sheet(frames, colors):
    width = max(len(r) for f in frames for r in f)
    out = []
    for rows in frames:
        art = _image([r.ljust(width, ".") for r in rows], colors, width)
        framed = pix.new_image((width + 3) & ~1, len(rows) + 2)   # the pak wants even widths
        pix.blit(framed, art, 1, 1)
        out.append(pix.mirror(pix.outline(framed, colors["o"])))
    return out


FROG = [                          # sitting, then mid-leap (facing right)
    [
        "...............",
        "...............",
        "...............",
        "......ww..ww...",
        ".....wewwwewL..",
        "....cccccccccL.",
        "...ccLLLLLLccs.",
        "..CccccccccMMM.",
        ".CCcccccccccc..",
        "CCCcLLLLLccC...",
        "CccCc..cc.Cc...",
        "kkkkk.kkk.kkk..",
    ],
    [
        "......ww..ww...",
        ".....wewwwewL..",
        "....cccccccccL.",
        "...ccLLLLLLccs.",
        ".CCccccccccMMM.",
        "CCcccccccccc...",
        "kkCcLLLLLccC...",
        "kk..cc..cc.cc..",
        "k....c...c..c..",
        ".....kk..kk.kk.",
        "...............",
        "...............",
    ],
]
PIGEON = [                        # four wingbeats (facing right)
    [
        ".....CC........",
        "....CLC........",
        "...CLLC........",
        "...CLC....hh...",
        "..CLC....hhehG.",
        ".CcccccccAAh...",
        "CCLLLLLLLcc....",
        ".CccccccccC....",
        "...CCCCCCC.....",
        ".....k..k......",
        "...............",
    ],
    [
        "...............",
        "...............",
        "...............",
        "..........hh...",
        ".........hhehG.",
        "CCCCCCcccAAh...",
        ".CLLLLLLLcc....",
        ".CccccccccC....",
        "...CCCCCCC.....",
        ".....k..k......",
        "...............",
    ],
    [
        "...............",
        "...............",
        "...............",
        "..........hh...",
        ".........hhehG.",
        "..cccccccAAh...",
        "CCLLLLLLLcc....",
        ".CccCCCCccC....",
        "...CCLLLCC.....",
        "....CLLC.......",
        ".....CC........",
    ],
    [
        "...............",
        "...............",
        "...............",
        "..........hh...",
        ".........hhehG.",
        "CCCCCCcccAAh...",
        ".CLLLLLLLcc....",
        ".CccccccccC....",
        "...CCCCCCC.....",
        ".....k..k......",
        "...............",
    ],
]
JALOPY = [                        # a runaway Model T, wheels turning (facing right)
    [
        ".....hhhhhhhhh........",
        ".....h.......h........",
        ".....h.......h........",
        ".....h.......hcc......",
        "..cccccccccccccLLwc...",
        ".ccLLLLLLLLLLLLLLLwwG.",
        ".cccccccccccccccccccc.",
        "..CCCbbbCCCCCCCCbbbCC.",
        "...bbAAAbb....bbAAAbb.",
        "...bAAwAAb....bAAwAAb.",
        "...bbAAAbb....bbAAAbb.",
        "....bbbbb......bbbbb..",
    ],
    [
        ".....hhhhhhhhh........",
        ".....h.......h........",
        ".....h.......h........",
        ".....h.......hcc......",
        "..cccccccccccccLLwc...",
        ".ccLLLLLLLLLLLLLLLwwG.",
        ".cccccccccccccccccccc.",
        "..CCCbbbCCCCCCCCbbbCC.",
        "...bbAwAbb....bbAwAbb.",
        "...bAAAAAb....bAAAAAb.",
        "...bbAwAbb....bbAwAbb.",
        "....bbbbb......bbbbb..",
    ],
]
PIE = [                           # a custard pie in flight (facing right)
    [
        "..wwwww.",
        ".wwwwwww",
        "SSSSSSSS",
        ".cccccc.",
    ],
]


def frog(colors):
    return _sheet(FROG, colors)


def pigeon(colors):
    return _sheet(PIGEON, colors)


def jalopy(colors):
    return _sheet(JALOPY, colors)


def pie(colors):
    return _sheet(PIE, colors)


def stage1():
    """The silent comedy's cast in sepia: Keystone cops, a pastry chef with
    custard pies, a bullfrog, pigeons, and a runaway jalopy."""
    sepia = lambda level: pix.gray(level, warm=1)
    cop = dress(sepia)
    chef = dress(sepia, c=13, C=10, L=15, w=8, p=4, P=3, T=7)
    frog_c = dress(sepia, c=6, C=4, L=9, w=14, M=2, k=5)
    bird = dress(sepia, c=8, C=5, L=11, h=6, A=12, e=1, G=13, k=4)
    car = dress(sepia, c=4, C=3, L=6, h=2, A=9, w=13, G=14)
    custard = dress(sepia, w=14, S=10, c=6)
    return {
        "walker": walker("cop", cop),
        "thrower": thrower("chef", chef),
        "hopper": frog(frog_c),
        "flyer": pigeon(bird),
        "charger": jalopy(car),
        "shot": pie(custard),
    }


def preview(path):
    cast = stage1()
    rows = []
    for name in ("walker", "thrower", "hopper", "flyer", "charger", "shot"):
        frames = cast[name]
        h = max(f.shape[0] for f in frames)
        strip = np.concatenate([np.pad(f, ((h - f.shape[0], 0), (0, 2)), constant_values=pix.TRANSPARENT)
                                for f in frames], axis=1)
        rows.append(strip)
    width = max(r.shape[1] for r in rows)
    sheet = np.concatenate([np.pad(r, ((0, 2), (0, width - r.shape[1])), constant_values=pix.TRANSPARENT)
                            for r in rows], axis=0)
    pix.write_png(path, sheet, scale=6, background=(150, 135, 115))


if __name__ == "__main__":
    import sys
    preview(sys.argv[1] if len(sys.argv) > 1 else "/tmp/retake-prev/cast.png")
