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
    "...ccccccc.qq.",
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
    "bandit": [                   # a ten-gallon hat and a bandana over the face
        "...hhhh...",
        "..hHHhhh..",
        "..hhhhhh..",
        "hhhhhhhhhh",
        "..Ssssss..",
        "..Sssesss.",
        "..AAAAAAA.",
        "..AAAAAAA.",
        "...AAAA...",
    ],
    "prospector": [               # a slouch hat and a great gray beard
        "..........",
        "...hhhh...",
        "..hhHHhh..",
        ".hhhhhhhh.",
        "..Ssssss..",
        "..Sssesss.",
        "..wwwwwwss",
        "..wwwwww..",
        "...wwww...",
    ],
    "skull": [                    # a tricorn over a grinning skull
        "..........",
        "....hh....",
        ".hhhhhhhh.",
        "hHhhhhhhHh",
        "..wwwwww..",
        "..wweweww.",
        "..wwwwwww.",
        "..wewewe..",
        "...wwww...",
    ],
    "buccaneer": [                # a knotted bandana, an eyepatch, a black beard
        "..........",
        "...AAAA...",
        "..AAAAAAA.",
        ".AAAAAAA..",
        "AA.Ssssss.",
        "A..SsMeMs.",
        "...SMsss..",
        "...MMMMs..",
        "....MMM...",
    ],
    "ghoul": [                    # bald, hollow-eyed, mouth agape
        "..........",
        "...ssss...",
        "..ssssss..",
        "..sSssss..",
        "..Sssssss.",
        "..SeSsess.",
        "..Ssssss..",
        "..SsMMMs..",
        "...SSSS...",
    ],
    "robot": [                    # a tin box head with an antenna and a visor
        "....A.....",
        "....h.....",
        "..hhhhhh..",
        "..hHHHHh..",
        "..hAAAAAh.",
        "..hAeAeAh.",
        "..hhhhhhh.",
        "..hhwhwhh.",
        "...hhhh...",
    ],
    "martian": [                  # a green head in a fishbowl helmet
        "...wwww...",
        ".ww....ww.",
        ".w.ssss.w.",
        "w.ssssss.w",
        "w.ssesse.w",
        "w.ssssss.w",
        ".w.sMMs.w.",
        "..wwwwww..",
        "..........",
    ],
    "fedora": [                   # a fedora with a band, the face in its shadow
        "..........",
        "...hhhh...",
        "..hhhhhh..",
        "..AAAAAA..",
        "hhhhhhhhh.",
        "..SSSSSS..",
        "..Sssesss.",
        "..Sssssss.",
        "...SsMS...",
    ],
    "tophat": [                   # a chorus dancer's top hat and a stage smile
        "..hhhhh...",
        "..hhhhh...",
        "..hHhhh...",
        "..AAAAA...",
        ".hhhhhhh..",
        "..Ssssss..",
        "..Sssesss.",
        "..SssMsss.",
        "...SsssS..",
    ],
    "shako": [                    # a bandsman's plumed shako
        "......w...",
        "..hhhhw...",
        "..hGGGh...",
        "..hhhhh...",
        ".hhhhhhhh.",
        "..Ssssss..",
        "..Sssesss.",
        "..Sssssss.",
        "...SsssS..",
    ],
}


def _image(rows, colors, width):
    art = pix.new_image(width, len(rows))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != "." and colors.get(ch) is not None:
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


def dress(tone, raw=None, **levels):
    """Role -> color for one costume. `tone(level)` is the picture's tint;
    `levels` overrides the default level of any role (None leaves the role
    out of the drawing), and `raw` gives exact colors for full-color stock."""
    base = {"o": 0, "c": 4, "C": 3, "L": 6, "p": 3, "P": 2, "b": 1, "G": 11, "w": 14,
            "s": 12, "S": 9, "e": 1, "M": 2, "h": 3, "H": 5, "T": 9, "k": 3, "A": 10,
            "q": None}
    base.update(levels)
    colors = {role: (None if level is None else tone(level)) for role, level in base.items()}
    colors.update(raw or {})
    return colors


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


def slate():
    """The clapper switch on its tripod, 16x24: sticks open while it waits,
    shut while it holds its switch. Plain black and white with a wood stand,
    so it reads the same in every picture's stock."""
    black, white, chalk = pix.rgb(1, 1, 1), pix.rgb(15, 15, 15), pix.rgb(11, 11, 11)
    wood, wood_dark, out = pix.rgb(9, 6, 3), pix.rgb(5, 3, 2), pix.rgb(0, 0, 0)
    frames = []
    for shut in (False, True):
        img = pix.new_image(16, 24)
        img[9:17, 2:14] = black                      # the board
        for y, x0, x1 in ((11, 3, 9), (13, 3, 12), (15, 3, 7), (15, 9, 12)):
            img[y, x0:x1] = chalk                    # scene, take, roll
        for x in range(2, 14):                       # the fixed stick
            for y in (7, 8):
                img[y, x] = white if ((x - y) // 2) % 2 else black
        for x in range(2, 14):                       # the hinged stick
            top = 5 if shut else 5 - ((x - 2) * 5) // 11
            for y in (top, top + 1):
                img[y, x] = white if ((x + y) // 2) % 2 else black
        for k in range(7):                           # the tripod
            img[17 + k, 7:9] = wood
            img[17 + k, max(1, 6 - k // 2 - k // 3)] = wood_dark
            img[17 + k, min(14, 9 + k // 2 + k // 3)] = wood_dark
        frames.append(pix.outline(img, out))
    return frames


def stage1():
    """The silent comedy's cast in sepia: Keystone cops, a pastry chef with
    custard pies, a bullfrog, pigeons, and a runaway jalopy."""
    sepia = lambda level: pix.gray(level, warm=1)
    cop = dress(sepia)
    chef = dress(sepia, c=13, C=10, L=15, w=8, p=4, P=3, T=7, q=14)
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
        "slate": slate(),
    }


def humanoids(tone, walker_head, walker_dress, thrower_head, thrower_dress):
    return {"walker": walker(walker_head, dress(tone, **walker_dress)),
            "thrower": thrower(thrower_head, dress(tone, **thrower_dress))}


def amber(level):
    level = max(0, min(15, level))
    return pix.rgb(min(15, level + (2 if level > 2 else 0)), level, max(0, level - 4))


def blue(level):
    level = max(0, min(15, level))
    return pix.rgb(max(0, level - 4), max(0, level - 2), min(15, level + 1))


def violet(level):
    level = max(0, min(15, level))
    return pix.rgb(level, max(0, level - 1), min(15, level + 1))


def night(level):
    level = max(0, min(15, int(level)))
    return pix.rgb(level, level, min(15, level + 1))


def stage_humanoids(stage):
    """Walker and thrower sheets for pictures 2-8 (stage 4's are animals)."""
    rgb = pix.rgb
    if stage == 2:   # the Western: a masked bandit, a prospector with dynamite
        return humanoids(amber, "bandit", dict(c=7, C=5, L=9, w=7, p=4, P=3, A=2, T=None),
                         "prospector", dict(c=5, C=4, L=7, w=13, p=6, P=4, T=12, q=15))
    if stage == 3:   # the pirate picture: a skeleton in a rotted coat, a buccaneer with a bomb
        return humanoids(blue, "skull", dict(c=4, C=3, L=6, w=14, p=5, P=4, s=13, S=10, T=12),
                         "buccaneer", dict(c=6, C=4, L=8, w=11, A=12, M=1, T=2, q=15))
    if stage == 5:   # the horror picture: a ghoul in grave clothes and a mad gravedigger
        return humanoids(violet, "ghoul", dict(c=5, C=3, L=7, w=5, s=11, S=7, e=0, T=None),
                         "ghoul", dict(c=3, C=2, L=5, w=3, s=11, S=7, e=0, T=12, q=15))
    if stage == 6:   # the serial: a tin robot, a Martian with a ray gun
        silver, steel, dark = rgb(12, 12, 13), rgb(8, 8, 10), rgb(5, 5, 7)
        cyan, lime, green, purple = rgb(6, 15, 15), rgb(10, 15, 4), rgb(4, 13, 5), rgb(8, 2, 10)
        tin = dress(night, raw={"o": rgb(2, 0, 3), "c": steel, "C": dark, "L": silver, "w": cyan,
                                "p": steel, "P": dark, "b": dark, "G": cyan, "h": steel,
                                "H": silver, "A": cyan, "e": rgb(1, 3, 4), "T": None, "k": dark,
                                "s": silver, "S": steel})
        mars = dress(night, raw={"o": rgb(2, 0, 3), "c": purple, "C": rgb(5, 1, 7),
                                 "L": rgb(11, 4, 13), "w": rgb(14, 15, 15), "p": purple,
                                 "P": rgb(5, 1, 7), "b": rgb(3, 1, 4), "G": cyan, "s": lime,
                                 "S": green, "M": rgb(2, 6, 2), "T": rgb(15, 8, 1),
                                 "q": rgb(15, 14, 3), "k": rgb(5, 1, 7)})
        return {"walker": walker("robot", tin), "thrower": thrower("martian", mars)}
    if stage == 7:   # the noir: hoods in fedoras and trench coats
        pink = rgb(15, 4, 11)
        coat = dress(night, raw={"A": pink}, c=6, C=4, L=9, w=12, p=3, P=2, s=12, S=7, T=None)
        bottle = dress(night, raw={"A": pink, "T": rgb(4, 9, 6), "q": rgb(9, 14, 10)},
                       c=5, C=3, L=8, w=12, p=3, P=2, s=12, S=7)
        return {"walker": walker("fedora", coat), "thrower": thrower("fedora", bottle)}
    if stage == 8:   # the musical: chorus dancers in tails, a bandsman
        black, white, gold = rgb(3, 2, 4), rgb(15, 15, 15), rgb(15, 12, 3)
        red, skin = rgb(14, 2, 3), rgb(15, 11, 8)
        tails = dress(night, raw={"o": rgb(2, 0, 1), "c": black, "C": rgb(2, 1, 3),
                                  "L": rgb(6, 5, 8), "w": white, "p": black, "P": rgb(2, 1, 3),
                                  "b": rgb(1, 1, 1), "G": gold, "h": black, "H": rgb(6, 5, 8),
                                  "A": red, "s": skin, "S": rgb(12, 8, 6), "e": rgb(1, 1, 2),
                                  "M": rgb(11, 4, 4), "T": white, "k": rgb(2, 1, 3)})
        band = dress(night, raw={"o": rgb(2, 0, 1), "c": red, "C": rgb(9, 1, 2),
                                 "L": rgb(15, 6, 6), "w": gold, "p": white,
                                 "P": rgb(11, 11, 12), "b": rgb(1, 1, 1), "G": gold,
                                 "h": red, "H": rgb(15, 6, 6), "s": skin,
                                 "S": rgb(12, 8, 6), "e": rgb(1, 1, 2), "T": gold,
                                 "q": rgb(15, 15, 10), "k": rgb(9, 1, 2)})
        return {"walker": walker("tophat", tails), "thrower": thrower("shako", band)}
    raise ValueError(f"cast.py: picture {stage} has no humanoid cast")


def preview_all(path):
    rows = []
    for stage in (2, 3, 5, 6, 7, 8):
        cast = stage_humanoids(stage)
        frames = cast["walker"] + cast["thrower"]
        rows.append(np.concatenate([np.pad(f, ((0, 0), (0, 2)), constant_values=pix.TRANSPARENT)
                                    for f in frames], axis=1))
    sheet = np.concatenate([np.pad(r, ((0, 2), (0, 0)), constant_values=pix.TRANSPARENT) for r in rows], axis=0)
    pix.write_png(path, sheet, scale=5, background=(120, 110, 100))


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
