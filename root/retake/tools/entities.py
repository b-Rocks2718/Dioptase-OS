"""Entity kinds for room files: names, codes (mirroring ENT_* in game.h) and
which kinds are persistent (collected once, tracked by a save flag).

Room file syntax:  <name> <tx> <ty> [a=..] [b=..] [c=..] [d=..]
Script and room references may be given by name: a=@intro (script) or
a=#stage1_entry (room); build.py resolves them.
"""

KINDS = {
    "coin": 1, "heart": 2, "frame": 3, "reel": 4, "ability": 5, "mirror": 6,
    "sign": 7, "npc": 8, "plate": 9, "gate": 10, "door": 11, "walker": 12,
    "hopper": 13, "flyer": 14, "thrower": 15, "charger": 16, "shot": 17,
    "sandbag": 18, "spring": 19, "boss": 20, "shop": 21, "wave": 22, "platform": 23, "tentacle": 24, "crumble": 25, "pendulum": 26, "bolt": 27, "laser": 28, "searchlight": 29, "beat": 30, "slate": 31,
}

PERSISTENT = {"coin", "frame", "reel", "ability"}

ABILITIES = {"jumpcut": 1, "wire": 2, "glide": 4, "rewind": 8, "overcrank": 16}
