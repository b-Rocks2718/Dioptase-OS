"""The authored world: the Backlot hub and the stages, built with roomdsl.

Movement metrics measured from Astra's controller (player.c, natively):
a full jump rises 46 px (just under 6 tiles) and carries 88 px (11 tiles)
at a run; with the stunt wire, 93 px up and 154 px across. Hence:
  - routine ledges are at most 5 tiles above the floor below them,
  - gaps on the main path are at most 7 tiles,
  - a 6-tile wall is the "stand on your own take" height (46 + 12 >= 48).
Regions: 0 = the Backlot, 1 = Stage 1 "Safety Last Street".
"""
from roomdsl import Draft


def backlot():
    out = []
    # ---- West lot: the main gate and Stages 1-4 ----
    r = Draft("backlot_west", 0, (0, 0), (80, 30))
    r.ground(26)
    r.wall(0, 0, 26)
    for i, x in enumerate((8, 26, 44, 62)):
        n = i + 1
        r.back(x, 7, x + 14, 26)
        r.stamp(x + 4, 20, str(n))
        r.stamp(x + 2, 10, "W")
        r.stamp(x + 8, 10, "w")
        if n == 1:
            r.ent("door", x + 6, 25, a="#s1_entry", b=6, c=25, d=1)
        else:
            r.ent("sign", x + 7, 25, a="@stage_sealed")
    for x in (23, 41, 59, 77):
        r.stamp(x - 1, 18, "P")
    r.stamp(3, 21, "L")
    r.ent("sign", 5, 25, a="@backlot_gate")
    r.ent("mirror", 3, 25)
    for x in (21, 24, 39, 42, 57, 60):
        r.ent("coin", x, 25)
    r.spawn(2, 25)
    out.append(r.done())

    # ---- East lot: Stages 5-8 and the Projection Tower ----
    r = Draft("backlot_east", 0, (80, 0), (80, 30))
    r.ground(26)
    r.wall(79, 0, 26)
    for i, x in enumerate((2, 18, 34, 50)):
        r.back(x, 7, x + 14, 26)
        r.stamp(x + 4, 20, str(i + 5))
        r.stamp(x + 2, 10, "w")
        r.stamp(x + 8, 10, "W")
        r.ent("sign", x + 7, 25, a="@stage_sealed")
    r.stamp(68, 11, "X")
    r.ent("sign", 71, 25, a="@tower_sealed")
    r.stamp(16, 18, "P")
    r.stamp(48, 18, "P")
    r.stamp(65, 21, "L")
    out.append(r.done())
    return out


def stage1():
    out = []
    # ---- Arrival from the stage door ----
    r = Draft("s1_entry", 1, (0, 30), (40, 30))
    r.ground(26)
    r.clear(30, 26, 34, 30)
    r.plank(30, 26, 4)                         # a trapdoor down to the cellar
    r.wall(0, 0, 26, w=2)
    r.building(2, 8, 15, 26, windows="Ww", door=None)
    r.building(18, 12, 28, 26, windows="W", sign=(19, 13, "F"))
    r.plank(16, 21, 5)
    r.ent("door", 3, 25, a="#backlot_west", b=15, c=25)
    r.ent("sign", 9, 25, a="@s1_title")
    r.ent("mirror", 12, 25)
    r.ent("coin", 17, 20)
    r.ent("coin", 19, 20)
    r.ent("sign", 24, 25, a="@s1_clap_hint")
    r.ent("walker", 36, 25)
    r.stamp(28, 21, "L")
    r.spawn(6, 25)
    out.append(r.done())

    # ---- Secret cellar under the trapdoor: climb out by wall kicks ----
    r = Draft("s1_cellar", 1, (0, 60), (40, 20))
    r.fill(0, 0, 40, 2)
    r.clear(30, 0, 34, 2)
    r.wall(0, 0, 20, w=2)
    r.wall(38, 0, 20, w=2)
    r.ground(16)
    r.wall(29, 2, 12)                          # chimney walls for wall kicks
    r.wall(34, 2, 12)
    r.back(2, 2, 29, 16)
    r.ent("frame", 6, 15)
    r.ent("reel", 14, 15, a="@s1_reel_a")
    r.ent("coin", 20, 15)
    r.ent("coin", 22, 15)
    r.ent("coin", 24, 15)
    r.ent("hopper", 18, 15)
    out.append(r.done())

    # ---- The street: first enemies, a nail pit, a falling sandbag ----
    r = Draft("s1_street", 1, (40, 30), (80, 30))
    r.ground(26)
    r.clear(22, 26, 28, 28)
    r.nails(22, 28, 6)
    r.building(0, 10, 14, 26, windows="Ww")
    r.building(18, 6, 30, 26, windows="wW", sign=(21, 9, "H"))
    r.building(34, 12, 50, 26, windows="W")
    r.building(54, 8, 70, 26, windows="Ww", sign=(58, 22, "K"))
    r.building(72, 14, 80, 26, windows="w")
    r.girder(12, 19, 5)
    r.plank(30, 20, 6)
    r.girder(44, 15, 6)
    r.stamp(32, 21, "L")
    r.stamp(52, 21, "L")
    r.ent("walker", 16, 25)
    r.ent("walker", 42, 25)
    r.ent("sandbag", 46, 16)
    r.ent("flyer", 60, 9, a=8)
    r.ent("thrower", 68, 25)
    for x in (31, 33, 35):
        r.ent("coin", x, 19)
    r.ent("coin", 45, 14)
    r.ent("coin", 47, 14)
    r.ent("heart", 13, 18)
    out.append(r.done())

    # ---- Hit your mark: the first take puzzle ----
    r = Draft("s1_mark", 1, (120, 30), (40, 30))
    r.ground(26)
    r.wall(30, 0, 19)
    r.ent("gate", 30, 19, a=1, b=7)
    r.building(0, 10, 28, 26, windows="Ww", sign=(4, 22, "F"))
    r.building(32, 12, 40, 26, windows="W")
    r.ent("sign", 12, 25, a="@s1_mark_hint")
    r.ent("plate", 20, 25, a=1)
    r.ent("coin", 34, 25)
    r.ent("coin", 36, 25)
    out.append(r.done())

    # ---- The scaffold: a climb with a wall-kick chimney ----
    r = Draft("s1_scaffold", 1, (160, 0), (40, 60))
    r.ground(56)
    r.fill(0, 0, 40, 2)
    r.wall(0, 2, 48, w=2)
    r.wall(38, 12, 56, w=2)
    r.back(2, 2, 38, 56)
    r.plank(4, 51, 5)
    r.plank(12, 47, 5)
    r.plank(20, 43, 5)
    r.plank(27, 39, 7)
    r.wall(27, 16, 38)                         # chimney: kick up between x=28..33
    r.wall(34, 16, 38)
    r.girder(28, 15, 10)
    r.plank(8, 34, 6)                          # a side ledge with a reward
    r.plank(14, 28, 5)
    r.ent("coin", 9, 33)
    r.ent("coin", 11, 33)
    r.ent("coin", 15, 27)
    r.ent("heart", 17, 27)
    r.ent("sandbag", 14, 48)
    r.ent("sandbag", 22, 44)
    r.ent("flyer", 16, 22, a=6)
    r.ent("sign", 6, 55, a="@s1_wall_hint")
    r.stamp(10, 4, "C")
    out.append(r.done())

    # ---- Two doubles are better than one ----
    r = Draft("s1_double", 1, (200, 0), (40, 30))
    r.fill(0, 15, 26, 30)                      # the floor Ruby arrives on
    r.fill(26, 9, 40, 30)                      # a ledge 6 tiles up: needs a take
    r.fill(0, 0, 40, 2)
    r.building(0, 2, 40, 15, windows="Ww")
    r.ent("sign", 8, 14, a="@s1_double_hint")
    r.ent("coin", 30, 8)
    r.ent("coin", 33, 8)
    r.ent("walker", 32, 8)
    out.append(r.done())

    # ---- Rooftops: gaps, pigeons, a pie-throwing baker, a mattress ----
    r = Draft("s1_rooftops", 1, (240, 0), (80, 30))
    r.fill(0, 9, 14, 30)
    r.fill(19, 11, 30, 30)
    r.fill(35, 9, 44, 30)
    r.fill(49, 13, 60, 30)
    r.fill(65, 10, 80, 30)
    r.girder(54, 4, 5)                         # high perch with a lost reel
    r.ent("spring", 52, 12)
    r.ent("reel", 56, 3, a="@s1_reel_b")
    r.ent("flyer", 16, 6, a=5)
    r.ent("flyer", 46, 6, a=5)
    r.ent("thrower", 40, 8)
    r.ent("walker", 24, 10)
    r.ent("walker", 70, 9)
    for x in (16, 17, 32, 33, 46, 47, 62, 63):
        r.ent("coin", x, 7)
    r.stamp(66, 1, "C")
    out.append(r.done())

    # ---- The clock tower's base: rest, and a comedian who can't stop ----
    r = Draft("s1_clock_base", 1, (320, 0), (40, 30))
    r.fill(0, 10, 6, 30)
    r.ground(20)
    r.fill(0, 0, 40, 2)
    r.clear(18, 0, 22, 2)                      # the way up
    r.wall(38, 2, 20, w=2)
    r.back(6, 2, 38, 20)
    r.stamp(16, 4, "C")
    r.plank(17, 14, 6)
    r.plank(17, 9, 6)
    r.plank(17, 4, 6)
    r.ent("mirror", 10, 19)
    r.ent("npc", 28, 19, a="@s1_comedian")
    r.ent("heart", 34, 19)
    out.append(r.done())

    # ---- Inside the clock: gears, a mark that holds the escapement ----
    r = Draft("s1_clock_climb", 1, (320, -60), (40, 60))
    r.wall(0, 0, 60, w=2)
    r.wall(38, 0, 60, w=2)
    r.fill(2, 58, 18, 60)
    r.fill(22, 58, 38, 60)
    r.back(2, 0, 38, 58)
    r.plank(16, 56, 8)                         # catches Ruby coming up from below
    r.girder(4, 50, 8)
    r.girder(26, 46, 8)
    r.plank(12, 42, 6)
    r.girder(4, 37, 6)
    r.ent("plate", 6, 36, a=2)                 # hold this mark with a take...
    r.fill(2, 30, 38, 32)                      # ...to open the escapement above
    r.clear(22, 30, 24, 32)
    r.ent("gate", 22, 30, a=2, b=2)
    r.ent("gate", 23, 30, a=2, b=2)
    r.plank(19, 35, 7)
    r.plank(24, 26, 6)
    r.girder(30, 21, 6)
    r.plank(16, 17, 6)
    r.girder(4, 13, 8)
    r.plank(16, 8, 8)
    r.plank(16, 4, 8)
    r.stamp(8, 44, "C")
    r.ent("sandbag", 28, 47)
    r.ent("flyer", 20, 24, a=6)
    r.ent("flyer", 12, 10, a=5)
    r.ent("coin", 6, 12)
    r.ent("coin", 8, 12)
    r.ent("reel", 35, 20, a="@s1_reel_c")
    out.append(r.done())

    # ---- The clock tower's roof: Sergeant Gearhart ----
    r = Draft("s1_clock_top", 1, (320, -90), (40, 30))
    r.wall(0, 0, 30, w=2)
    r.wall(38, 0, 30, w=2)
    r.fill(2, 26, 16, 30)
    r.fill(24, 26, 38, 30)
    r.plank(16, 26, 8)                         # the hatch Ruby climbs through
    r.fill(0, 0, 40, 2)
    r.girder(6, 18, 6)
    r.girder(28, 18, 6)
    r.back(2, 2, 38, 26)
    r.stamp(17, 6, "C")
    r.ent("boss", 30, 25, a=1)
    out.append(r.done())
    return out


def all_rooms():
    return backlot() + stage1()
