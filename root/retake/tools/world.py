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
        elif n == 2:
            r.ent("door", x + 6, 25, a="#s2_entry", b=6, c=25, d=1 | (1 << 4))
        elif n == 3:
            r.ent("door", x + 6, 25, a="#s3_entry", b=6, c=21, d=1 | (2 << 4))
        elif n == 4:
            r.ent("door", x + 6, 25, a="#s4_entry", b=6, c=25, d=1 | (3 << 4))
        else:
            r.ent("sign", x + 7, 25, a="@stage_sealed")
    for x in (23, 41, 59, 77):
        r.stamp(x - 1, 18, "P")
    r.stamp(3, 21, "L")
    r.ent("sign", 5, 25, a="@backlot_gate")
    r.ent("mirror", 3, 25)
    r.ent("shop", 22, 25)
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
        if i == 0:
            r.ent("door", x + 6, 25, a="#s5_gate", b=6, c=25, d=1 | (5 << 4))
        elif i == 1:
            r.ent("door", x + 6, 25, a="#s6_gate", b=6, c=25, d=1 | (6 << 4))
        elif i == 2:
            r.ent("door", x + 6, 25, a="#s7_gate", b=6, c=25, d=1 | (7 << 4))
        else:
            r.ent("door", x + 6, 25, a="#s8_gate", b=6, c=25, d=1 | (8 << 4))
    r.stamp(68, 11, "X")
    r.ent("door", 71, 25, a="#tower_base", b=5, c=25, d=1 | (9 << 4))
    r.stamp(16, 18, "P")
    r.stamp(48, 18, "P")
    r.stamp(65, 21, "L")
    out.append(r.done())
    return out


def stage1():
    """Teach recording, wall kicks and plate cooperation across the silent comedy set."""
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
    r.wall(27, 16, 35)                         # chimney: kick up between x=28..33,
    r.wall(34, 16, 38)                         # entered from below over the left wall
    r.plank(28, 15, 6)                         # a one-way roof Ruby can kick up through
    r.girder(34, 15, 4)
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
    r.plank(12, 37, 4)                        # transfer under the ceiling without spending the plate take
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


def stage2():
    """Stage 2, "Dust & Iron": Ruby now has the stunt wire (double jump)."""
    out = []
    # ---- The edge of town ----
    r = Draft("s2_entry", 2, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.stamp(20, 20, "C")
    r.stamp(30, 9, "M")
    r.plank(10, 21, 6)
    r.ent("door", 3, 25, a="#backlot_west", b=33, c=25)
    r.ent("sign", 8, 25, a="@s2_title")
    r.ent("mirror", 12, 25)
    r.ent("coin", 11, 20)
    r.ent("coin", 13, 20)
    r.ent("hopper", 30, 25)
    r.spawn(6, 25)
    out.append(r.done())

    # ---- Main street: saloon, jail, general store ----
    r = Draft("s2_main_street", 2, (40, 0), (80, 30))
    r.ground(26)
    r.back(2, 10, 20, 26)
    r.stamp(8, 22, "D")
    r.stamp(5, 12, "H")
    r.stamp(4, 15, "W")
    r.stamp(13, 15, "w")
    r.girder(2, 9, 18)                          # saloon roof
    r.back(26, 14, 40, 26)
    r.stamp(29, 16, "K")
    r.stamp(28, 20, "w")
    r.stamp(34, 20, "w")
    r.girder(26, 13, 14)                        # jail roof
    r.stamp(42, 21, "P")
    r.back(48, 12, 66, 26)
    r.stamp(52, 14, "F")
    r.stamp(50, 18, "W")
    r.stamp(58, 18, "W")
    r.girder(48, 11, 18)                        # store roof
    r.stamp(70, 4, "T")
    r.plank(20, 20, 6)
    r.plank(40, 18, 6)
    r.ent("npc", 32, 25, a="@s2_sheriff")
    r.ent("walker", 14, 25)
    r.ent("walker", 46, 25)
    r.ent("thrower", 56, 10)
    r.ent("flyer", 38, 6, a=8)
    r.ent("reel", 33, 12, a="@s2_reel_a")
    for x in (21, 23, 25, 41, 43, 45):
        r.ent("coin", x, 19 if x < 30 else 17)
    r.ent("heart", 62, 25)
    out.append(r.done())

    # ---- The canyon: trestles over a gorge (the bottom is a fatal drop) ----
    r = Draft("s2_canyon", 2, (120, 0), (80, 30))
    r.fill(0, 26, 12, 30)                       # left mesa, level with Main Street
    r.girder(12, 24, 6)
    r.girder(21, 21, 6)
    r.fill(30, 22, 40, 30)                      # the middle pillar, cactus on its rims
    r.nails(30, 21, 3)
    r.nails(37, 21, 3)
    r.girder(42, 19, 6)
    r.girder(50, 16, 6)
    r.fill(58, 20, 80, 30)                      # right mesa
    r.girder(30, 12, 8)                         # a high perch for the wire
    r.stamp(4, 18, "C")
    r.stamp(66, 10, "C")
    r.ent("coin", 31, 11)
    r.ent("coin", 33, 11)
    r.ent("frame", 36, 11)
    r.ent("charger", 70, 19)
    r.ent("hopper", 34, 21)
    r.ent("flyer", 26, 12, a=6)
    r.ent("flyer", 52, 9, a=6)
    out.append(r.done())

    # ---- The mine mouth ----
    r = Draft("s2_mine_mouth", 2, (200, 0), (40, 30))
    r.fill(0, 20, 26, 30)                       # level with the canyon's right mesa
    r.fill(30, 20, 40, 30)
    r.clear(26, 20, 30, 30)                     # the shaft down into the mine
    r.plank(26, 20, 4)
    r.wall(38, 0, 20, w=2)
    r.stamp(27, 9, "N")
    r.stamp(8, 12, "C")
    r.ent("mirror", 6, 19)
    r.ent("sign", 14, 19, a="@s2_cart_hint")
    r.ent("walker", 20, 19)
    out.append(r.done())

    # ---- The mine: carts over the dark ----
    r = Draft("s2_mine", 2, (200, 30), (80, 30))
    r.fill(0, 0, 80, 2)
    r.back(2, 2, 78, 26)
    r.fill(0, 22, 26, 30)
    r.clear(26, 0, 30, 2)                       # the way back up to the mouth
    r.fill(26, 26, 30, 30)
    r.fill(66, 26, 80, 30)                      # level with the shaft's floor
    r.ent("platform", 30, 22, a=10, b=0, c=120, d=4)
    r.ent("platform", 46, 18, a=8, b=0, c=150, d=4)
    r.girder(58, 14, 6)
    r.ent("reel", 61, 13, a="@s2_reel_b")
    r.ent("coin", 40, 15)
    r.ent("coin", 50, 12)
    r.ent("flyer", 40, 8, a=8)
    r.ent("hopper", 70, 21)
    r.wall(78, 2, 14, w=2)
    out.append(r.done())

    # ---- The shaft: elevator cages up to the rail yard ----
    r = Draft("s2_shaft", 2, (280, 0), (40, 60))
    r.wall(38, 12, 60, w=2)
    r.fill(0, 0, 40, 2)
    r.wall(0, 2, 52, w=2)                       # opening at the bottom-left from the mine
    r.ground(56)
    r.back(2, 2, 38, 56)
    r.ent("platform", 8, 40, a=12, b=1, c=110, d=4)
    r.girder(16, 38, 6)
    r.ent("platform", 26, 28, a=10, b=1, c=130, d=4)
    r.girder(14, 20, 6)
    r.girder(26, 12, 12)
    r.ent("sandbag", 18, 39)
    r.ent("coin", 17, 37)
    r.ent("coin", 19, 37)
    r.ent("flyer", 20, 26, a=6)
    out.append(r.done())

    # ---- The rail yard ----
    r = Draft("s2_rail_yard", 2, (320, 0), (80, 30))
    r.ground(26)
    r.fill(0, 0, 80, 2)
    r.block(0, 12, 6, 14)
    r.block(10, 20, 12, 6)                      # boxcars
    r.block(28, 18, 12, 8)
    r.block(46, 21, 10, 5)
    r.stamp(62, 6, "T")
    r.girder(60, 13, 8)
    r.ent("sign", 6, 25, a="@s2_brake_hint")
    r.ent("walker", 20, 19)
    r.ent("thrower", 34, 17)
    r.ent("charger", 58, 25)
    r.ent("reel", 64, 12, a="@s2_reel_c")
    r.ent("heart", 50, 20)
    out.append(r.done())

    # ---- The Iron Horse's yard ----
    r = Draft("s2_arena", 2, (400, 0), (60, 30))
    r.ground(26)
    r.fill(0, 0, 60, 2)
    r.wall(58, 2, 26, w=2)
    r.girder(4, 18, 8)
    r.girder(22, 15, 10)                        # the signal platform with the brake
    r.girder(46, 18, 8)
    r.ent("plate", 26, 14, a=3, b=1)
    r.ent("plate", 27, 14, a=3, b=1)
    r.stamp(52, 6, "M")
    r.ent("boss", 30, 25, a=2)
    out.append(r.done())
    return out


def stage3():
    """Stage 3, "The Crimson Tide": wire and jump cut in hand. The sea below
    every pier is a fatal drop (the rooms' bottom edges are pits)."""
    out = []
    # ---- The harbor ----
    r = Draft("s3_entry", 3, (0, 0), (40, 30))
    r.fill(0, 22, 26, 30)
    r.plank(26, 22, 14)
    r.wall(0, 0, 22, w=2)
    r.stamp(5, 13, "T")
    r.stamp(30, 10, "K")
    r.ent("door", 3, 21, a="#backlot_west", b=51, c=25)
    r.ent("mirror", 9, 21)
    r.ent("sign", 13, 21, a="@s3_title")
    r.ent("npc", 19, 21, a="@s3_captain")
    r.ent("coin", 30, 21)
    r.ent("coin", 33, 21)
    r.ent("flyer", 30, 8, a=6)
    r.spawn(6, 21)
    out.append(r.done())

    # ---- The docks: piers, crates and rafts over the water ----
    r = Draft("s3_docks", 3, (40, 0), (80, 30))
    r.plank(0, 22, 10)
    r.ent("platform", 11, 18, a=5, b=1, c=90, d=4)
    r.plank(16, 22, 12)
    r.block(20, 19, 3, 3)
    r.ent("platform", 28, 22, a=5, b=0, c=110, d=3)
    r.plank(36, 22, 10)
    r.nails(40, 21, 2)
    r.plank(46, 18, 8)
    r.plank(62, 22, 18)
    r.stamp(66, 12, "K")
    r.ent("walker", 25, 21)
    r.ent("flyer", 34, 10, a=6)
    r.ent("flyer", 58, 8, a=6)
    r.ent("thrower", 50, 17)
    r.ent("hopper", 44, 21)
    for x in (17, 18, 47, 49, 51, 64, 66):
        r.ent("coin", x, 21 if x < 40 or x > 60 else 17)
    out.append(r.done())

    # ---- The galleon's deck: rigging up the mainmast ----
    r = Draft("s3_deck", 3, (120, 0), (80, 30))
    r.fill(0, 22, 80, 30)
    r.clear(38, 22, 42, 30)
    r.plank(38, 22, 4)                          # the hatch to the hold
    r.block(60, 18, 20, 4)                      # the quarterdeck
    r.girder(24, 2, 1, 20)                      # the mainmast
    r.stamp(25, 3, "S")
    r.stamp(26, 0, "F")
    r.plank(18, 18, 5)
    r.plank(26, 15, 5)
    r.plank(18, 12, 5)
    r.plank(26, 9, 5)
    r.plank(18, 6, 5)                           # the crow's nest
    r.stamp(72, 13, "H")
    r.back(2, 14, 14, 22)
    r.stamp(4, 16, "W")
    r.stamp(9, 16, "w")
    r.ent("reel", 20, 5, a="@s3_reel_a")
    r.ent("walker", 12, 21)
    r.ent("walker", 50, 21)
    r.ent("thrower", 70, 17)
    r.ent("flyer", 40, 6, a=8)
    r.ent("heart", 66, 17)
    out.append(r.done())

    # ---- The hold: a dead end with a cache (back up through the hatch) ----
    r = Draft("s3_hold", 3, (120, 30), (80, 30))
    r.fill(0, 0, 80, 2)
    r.clear(38, 0, 42, 2)
    r.ground(26)
    r.wall(0, 2, 26, w=2)
    r.wall(78, 2, 26, w=2)
    r.back(2, 2, 78, 26)
    r.stamp(10, 8, "w")
    r.stamp(30, 8, "W")
    r.stamp(50, 8, "w")
    r.stamp(70, 8, "W")
    r.plank(36, 20, 8)                          # a step to jump back up the hatch
    r.ent("sign", 32, 25, a="@s3_cut_hint")
    r.ent("charger", 60, 25)
    r.ent("frame", 74, 25)
    r.ent("reel", 6, 25, a="@s3_reel_b")
    r.ent("coin", 20, 25)
    r.ent("coin", 22, 25)
    r.ent("coin", 24, 25)
    out.append(r.done())

    # ---- Walking the plank ----
    r = Draft("s3_plank", 3, (200, 0), (80, 30))
    r.fill(0, 22, 6, 30)
    r.plank(6, 22, 8)
    r.plank(18, 20, 6)
    r.plank(28, 17, 4)
    r.plank(36, 20, 6)
    r.nails(38, 19, 2)
    r.ent("platform", 44, 21, a=8, b=0, c=140, d=3)
    r.plank(58, 18, 6)
    r.plank(68, 22, 12)
    r.ent("flyer", 22, 10, a=6)
    r.ent("flyer", 50, 9, a=8)
    r.ent("thrower", 60, 17)
    for x in (20, 22, 29, 30, 59, 61):
        r.ent("coin", x, 19 if x < 25 else 16 if x < 35 else 17)
    out.append(r.done())

    # ---- The wreck on the rocks ----
    r = Draft("s3_wreck", 3, (280, 0), (80, 30))
    r.plank(0, 22, 10)
    r.block(10, 20, 8, 10)
    r.block(22, 16, 6, 14)
    r.girder(30, 12, 2, 6)
    r.block(34, 18, 10, 12)
    r.ent("platform", 46, 18, a=6, b=1, c=80, d=4)
    r.block(54, 14, 6, 16)
    r.block(64, 22, 16, 8)
    r.girder(28, 6, 6)
    r.ent("reel", 30, 5, a="@s3_reel_c")
    r.ent("mirror", 70, 21)
    r.ent("walker", 38, 17)
    r.ent("hopper", 24, 15)
    r.ent("flyer", 50, 6, a=6)
    r.ent("coin", 56, 13)
    r.ent("coin", 58, 13)
    out.append(r.done())

    # ---- The Kraken's raft ----
    r = Draft("s3_arena", 3, (360, 0), (60, 30))
    r.fill(4, 24, 56, 30)
    r.girder(10, 16, 8)
    r.girder(42, 16, 8)
    r.girder(26, 10, 8)
    r.fill(0, 0, 60, 2)
    r.wall(58, 2, 24, w=2)
    r.ent("boss", 30, 23, a=3)
    out.append(r.done())
    return out


def stage4():
    """Stage 4, "Jungle Serial, Chapter 12": crumbling temple stones, vine
    bridges, thorns; wire, jump cut and umbrella in hand."""
    out = []
    r = Draft("s4_entry", 4, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.stamp(20, 18, "P")
    r.stamp(30, 10, "Q")
    r.ent("door", 3, 25, a="#backlot_west", b=69, c=25)
    r.ent("mirror", 8, 25)
    r.ent("sign", 12, 25, a="@s4_title")
    r.ent("npc", 16, 25, a="@s4_narrator")
    r.ent("hopper", 30, 25)
    r.spawn(6, 25)
    out.append(r.done())

    r = Draft("s4_canopy", 4, (40, 0), (80, 30))
    r.ground(26, 0, 10)
    r.nails(10, 29, 60)
    r.ground(26, 70, 80)
    r.plank(10, 22, 8)
    r.girder(20, 18, 6)
    r.plank(28, 20, 10)
    r.girder(40, 15, 6)
    r.plank(48, 18, 10)
    r.girder(60, 21, 10)
    r.stamp(4, 18, "P")
    r.stamp(34, 4, "Q")
    r.stamp(64, 6, "Q")
    r.ent("flyer", 24, 10, a=6)
    r.ent("flyer", 52, 9, a=6)
    r.ent("walker", 32, 19)
    r.ent("thrower", 43, 14)
    r.ent("reel", 42, 6, a="@s4_reel_a")
    r.girder(40, 7, 6)
    for x in (12, 14, 29, 31, 50, 52, 62, 64):
        r.ent("coin", x, 21 if x < 20 else 19 if x < 40 else 17 if x < 60 else 20)
    out.append(r.done())

    r = Draft("s4_ruins", 4, (120, 0), (80, 30))
    r.ground(26, 0, 8)
    r.nails(8, 29, 64)
    r.ground(26, 72, 80)
    for i, x in enumerate(range(8, 72, 4)):
        y = 24 - (i % 3) * 2
        r.ent("crumble", x, y)
        r.ent("crumble", x + 1, y)
    r.back(20, 6, 60, 22)
    r.stamp(36, 9, "I")
    r.ent("sign", 4, 25, a="@s4_crumble_hint")
    r.ent("flyer", 30, 12, a=8)
    r.ent("flyer", 56, 10, a=8)
    r.ent("heart", 74, 25)
    out.append(r.done())

    r = Draft("s4_temple_gate", 4, (200, 0), (40, 30))
    r.ground(26)
    r.back(6, 4, 34, 26)
    r.stamp(16, 8, "I")
    r.wall(30, 0, 20, w=2)
    r.ent("gate", 30, 20, a=6, b=6)
    r.ent("gate", 31, 20, a=6, b=6)
    r.ent("plate", 18, 25, a=6)
    r.ent("sign", 6, 25, a="@s4_gate_hint")
    r.ent("coin", 34, 25)
    r.ent("coin", 36, 25)
    out.append(r.done())

    r = Draft("s4_temple", 4, (240, 0), (80, 30))
    r.ground(26)
    r.fill(0, 0, 80, 2)
    r.back(2, 2, 78, 26)
    r.stamp(10, 6, "I")
    r.stamp(60, 6, "I")
    r.nails(20, 25, 8)
    r.ent("crumble", 20, 22)
    r.ent("crumble", 21, 22)
    r.ent("crumble", 24, 21)
    r.ent("crumble", 25, 21)
    r.girder(30, 18, 6)
    r.nails(40, 25, 12)
    r.ent("platform", 40, 20, a=8, b=0, c=120, d=3)
    r.girder(54, 16, 6)
    r.block(64, 18, 6, 8)
    r.ent("walker", 14, 25)
    r.ent("charger", 70, 17)
    r.ent("hopper", 34, 17)
    r.ent("thrower", 56, 15)
    r.ent("reel", 66, 17, a="@s4_reel_b")
    r.ent("frame", 32, 17)
    r.wall(78, 2, 18, w=2)
    out.append(r.done())

    r = Draft("s4_falls", 4, (320, -30), (40, 60))
    r.wall(0, 0, 48, w=2)
    r.wall(38, 12, 60, w=2)
    r.fill(0, 0, 40, 2)
    r.ground(56)
    r.back(2, 2, 38, 56)
    r.ent("platform", 6, 40, a=10, b=1, c=110, d=4)
    r.girder(16, 38, 6)
    r.ent("crumble", 26, 32)
    r.ent("crumble", 27, 32)
    r.girder(30, 26, 6)
    r.ent("crumble", 20, 22)
    r.ent("crumble", 21, 22)
    r.girder(8, 18, 6)
    r.girder(18, 12, 20)
    r.ent("flyer", 20, 30, a=6)
    r.ent("reel", 10, 17, a="@s4_reel_c")
    r.ent("mirror", 34, 11)
    out.append(r.done())

    r = Draft("s4_arena", 4, (360, -30), (60, 30))
    r.ground(26)
    r.fill(0, 0, 60, 2)
    r.wall(58, 2, 26, w=2)
    r.girder(8, 18, 8)
    r.girder(44, 18, 8)
    r.girder(26, 12, 8)
    r.stamp(28, 4, "I")
    r.ent("boss", 30, 25, a=5)
    out.append(r.done())
    return out


def stage5():
    """Stage 5, "The Monster of Hollow Manor": pendulums, crumbling stairs,
    graveyard ghouls; everything Ruby has learned so far."""
    out = []
    r = Draft("s5_gate", 5, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.stamp(22, 18, "R")
    r.stamp(30, 20, "G")
    r.stamp(8, 21, "L")
    r.ent("door", 3, 25, a="#backlot_east", b=9, c=25)
    r.ent("mirror", 12, 25)
    r.ent("sign", 16, 25, a="@s5_title")
    r.ent("flyer", 28, 10, a=6)
    r.spawn(6, 25)
    out.append(r.done())

    r = Draft("s5_graveyard", 5, (40, 0), (80, 30))
    r.ground(26)
    r.clear(30, 26, 36, 28)
    r.nails(30, 28, 6)
    r.clear(52, 26, 58, 28)
    r.nails(52, 28, 6)
    for x in (6, 16, 40, 46, 62, 70):
        r.stamp(x, 21, "G")
    r.stamp(24, 10, "R")
    r.stamp(66, 12, "R")
    r.girder(26, 18, 6)
    r.girder(48, 17, 6)
    r.ent("walker", 12, 25)
    r.ent("walker", 44, 25)
    r.ent("walker", 66, 25)
    r.ent("hopper", 28, 17)
    r.ent("flyer", 34, 8, a=8)
    r.ent("flyer", 58, 7, a=8)
    for x in (27, 29, 49, 51):
        r.ent("coin", x, 17 if x < 40 else 16)
    out.append(r.done())

    r = Draft("s5_hall", 5, (120, 0), (80, 30))
    r.ground(26)
    r.fill(0, 0, 80, 2)
    r.back(2, 2, 78, 26)
    for x in (8, 26, 44, 62):
        r.stamp(x, 6, "P")
    # Three long blades graze the walkway (lowest point ~4 px above the
    # floor), a third of a swing apart; the gaps between them are safe.
    # The girder starts past the last blade's reach at standing height.
    r.ent("pendulum", 16, 2, a=22, b=0, c=4)
    r.ent("pendulum", 32, 2, a=22, b=11, c=4)
    r.ent("pendulum", 48, 2, a=22, b=22, c=4)
    r.girder(67, 18, 8)
    r.ent("sign", 6, 25, a="@s5_pendulum_hint")
    r.ent("thrower", 69, 17)
    r.ent("charger", 74, 25)
    r.ent("heart", 72, 17)
    out.append(r.done())

    # Placed so its bottom passage meets the hall's floor on the left and the
    # lab's floor on the right; the climb above is the way to the reel.
    r = Draft("s5_library", 5, (200, -30), (40, 60))
    r.wall(0, 0, 52, w=2)
    r.wall(38, 0, 46, w=2)
    r.ground(56)
    r.fill(0, 0, 40, 2)
    r.back(2, 2, 38, 56)
    r.girder(26, 50, 10)
    r.ent("crumble", 18, 46)
    r.ent("crumble", 19, 46)
    r.girder(4, 42, 10)
    r.ent("pendulum", 20, 30, a=6, b=8, c=2)
    r.girder(26, 36, 10)
    r.ent("crumble", 14, 30)
    r.ent("crumble", 15, 30)
    r.girder(4, 24, 8)
    r.girder(16, 18, 6)
    r.girder(28, 12, 10)
    r.stamp(8, 8, "P")
    r.ent("flyer", 20, 40, a=8)
    r.ent("hopper", 8, 41)
    r.ent("reel", 6, 23, a="@s5_reel_a")
    r.ent("coin", 30, 11)
    r.ent("coin", 33, 11)
    out.append(r.done())

    r = Draft("s5_lab", 5, (240, 0), (80, 30))
    r.ground(26)
    r.clear(6, 26, 10, 30)
    r.plank(6, 26, 4)                           # a trapdoor down to the crypt
    r.fill(0, 0, 80, 2)
    r.back(2, 2, 78, 26)
    for x in (12, 30, 48):
        r.stamp(x, 21, "Z")
    r.nails(18, 25, 6)
    r.nails(36, 25, 6)
    r.girder(16, 20, 10)
    r.girder(34, 19, 10)
    r.ent("platform", 54, 20, a=10, b=0, c=110, d=3)
    r.ent("npc", 12, 25, a="@s5_igor")
    r.ent("mirror", 70, 25)
    r.ent("reel", 40, 18, a="@s5_reel_b")
    r.ent("walker", 26, 25)
    r.ent("thrower", 46, 25)
    r.ent("flyer", 60, 8, a=8)
    r.ent("frame", 20, 19)
    out.append(r.done())

    r = Draft("s5_crypt", 5, (240, 30), (40, 30))
    r.fill(0, 0, 40, 2)
    r.ground(26)
    r.wall(0, 2, 26, w=2)
    r.wall(38, 2, 26, w=2)
    r.back(2, 2, 38, 26)
    r.clear(6, 0, 10, 2)                        # back up to the lab's trapdoor
    for y in (22, 17, 12, 7, 3):
        r.plank(6, y, 4)
    r.ent("reel", 30, 25, a="@s5_reel_c")
    r.ent("coin", 12, 25)
    r.ent("coin", 14, 25)
    r.ent("walker", 20, 25)
    out.append(r.done())

    r = Draft("s5_arena", 5, (320, 0), (60, 30))
    r.ground(26)
    r.fill(0, 0, 60, 2)
    r.wall(58, 2, 26, w=2)
    r.back(2, 2, 58, 26)
    r.stamp(8, 21, "Z")
    r.stamp(48, 21, "Z")
    r.girder(10, 17, 8)
    r.girder(42, 17, 8)
    r.ent("boss", 30, 25, a=6)
    out.append(r.done())
    return out


def laser(r, x, y, length, on, off, phase=0, width=1, switch=None):
    """A ray curtain whose emitter sits in tile (x, y): the beam runs
    `length` tiles down from the tile below and is `width` tiles wide. It
    burns for `on` ticks and rests for `off` (on=0: always burning); while
    `switch` is held down the curtain is dark. Packed into d as phase |
    (width-1) << 8 | (switch+1) << 12, which must fit an int16."""
    assert 1 <= width <= 16 and 0 <= phase < 256 and (switch is None or 0 <= switch < 7)
    d = phase | ((width - 1) << 8) | ((0 if switch is None else switch + 1) << 12)
    r.ent("laser", x, y, a=length, b=on, c=off, d=d)


def stage6():
    """Stage 6, "Rocket Rangers of the Moon": moon gravity, ray curtains
    (one too quick to pass without Overcrank, one held dark by a take on a
    plate), saucers on wires, hover pads over a crystal pit."""
    out = []
    r = Draft("s6_gate", 6, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.stamp(28, 19, "K")
    r.stamp(17, 24, "C")
    r.stamp(23, 23, "D")
    r.stamp(34, 23, "Y")
    r.girder(20, 17, 5)                         # a first moon hop
    r.ent("door", 3, 25, a="#backlot_east", b=25, c=25)
    r.ent("mirror", 8, 25)
    r.ent("sign", 12, 25, a="@s6_title")
    r.ent("sign", 15, 25, a="@s6_gravity_hint")
    r.ent("coin", 21, 16)
    r.ent("coin", 23, 16)
    r.ent("flyer", 31, 9, a=6)
    r.spawn(6, 25)
    out.append(r.done())

    r = Draft("s6_craters", 6, (40, 0), (80, 30))
    r.ground(26)
    for x0, x1 in ((10, 22), (32, 42), (58, 72)):
        r.clear(x0, 26, x1, 28)
        r.nails(x0, 28, x1 - x0)
    r.girder(64, 20, 3)                         # a stepping stone over the widest crater
    r.clear(46, 26, 50, 30)
    r.plank(46, 26, 4)                          # a hatch down to the crater cave
    for x in (4, 25, 54, 76):
        r.stamp(x, 23, "Y")
    r.stamp(28, 23, "D")
    r.ent("walker", 27, 25)
    r.ent("walker", 53, 25)
    r.ent("hopper", 44, 25)
    r.ent("flyer", 37, 9, a=8)
    r.ent("flyer", 66, 8, a=8)
    for x, y in ((14, 19), (16, 18), (18, 19), (35, 19), (37, 18), (39, 19)):
        r.ent("coin", x, y)
    out.append(r.done())

    r = Draft("s6_cave", 6, (60, 30), (40, 20))
    r.fill(0, 0, 40, 2)
    r.clear(26, 0, 30, 2)                       # the hatch above
    r.ground(16)
    r.wall(0, 2, 16, w=2)
    r.wall(38, 2, 16, w=2)
    r.back(2, 2, 38, 16)
    for y in (12, 7, 3):                        # grating rungs back up
        r.plank(26, y, 4)
    r.stamp(8, 13, "Y")
    r.ent("reel", 33, 15, a="@s6_reel_a")
    r.ent("frame", 6, 15)
    r.ent("hopper", 16, 15)
    r.ent("coin", 12, 15)
    r.ent("coin", 20, 15)
    out.append(r.done())

    r = Draft("s6_lasers", 6, (120, 0), (60, 30))
    r.ground(26)
    r.fill(0, 0, 60, 2)
    r.back(0, 2, 60, 26)
    for x in (4, 27, 52):
        r.stamp(x, 6, "O")
    r.ent("sign", 6, 25, a="@s6_laser_hint")
    laser(r, 12, 2, 23, 50, 50)
    laser(r, 20, 2, 23, 40, 30, phase=20, width=3)
    laser(r, 32, 2, 23, 40, 12, width=3)        # too quick to outrun: Overcrank
    laser(r, 43, 2, 23, 24, 24, phase=12, width=2)
    r.stamp(48, 24, "C")
    r.ent("heart", 28, 25)
    r.ent("thrower", 55, 25)
    out.append(r.done())

    r = Draft("s6_hangar", 6, (180, 0), (80, 30))
    r.fill(0, 0, 80, 2)
    r.ground(26, 0, 10)
    r.ground(26, 70, 80)
    r.nails(10, 29, 60)
    r.back(0, 2, 80, 26)
    r.girder(28, 21, 4)
    r.girder(50, 19, 4)
    r.ent("platform", 12, 22, a=12, b=0, c=120, d=3)
    r.ent("platform", 36, 16, a=8, b=1, c=90, d=3)
    r.ent("platform", 56, 22, a=10, b=0, c=140, d=3)
    r.ent("flyer", 22, 9, a=8)
    r.ent("flyer", 46, 7, a=8)
    r.ent("flyer", 64, 10, a=8)
    r.stamp(73, 19, "K")
    r.ent("coin", 29, 20)
    r.ent("coin", 51, 18)
    out.append(r.done())

    r = Draft("s6_reactor", 6, (260, -30), (40, 60))
    r.fill(0, 0, 40, 2)
    r.ground(56)
    r.wall(0, 2, 48, w=2)                       # the hangar joins at the bottom left
    r.wall(38, 2, 18, w=2)
    r.wall(38, 27, 56, w=2)                     # the observatory joins at the top right
    r.back(0, 2, 40, 56)
    r.ent("spring", 8, 55)
    r.girder(4, 46, 8)
    r.girder(26, 40, 8)
    r.girder(4, 33, 8)
    r.girder(18, 26, 6)
    r.girder(30, 26, 8)
    laser(r, 19, 27, 29, 60, 40, width=2)       # splits the shaft below the top deck
    r.stamp(6, 52, "C")
    r.ent("flyer", 30, 48, a=6)
    r.ent("flyer", 12, 20, a=6)
    for x, y in ((28, 39), (30, 39), (6, 32), (8, 32)):
        r.ent("coin", x, y)
    out.append(r.done())

    r = Draft("s6_observatory", 6, (300, -30), (40, 30))
    r.fill(0, 0, 40, 2)
    r.ground(26)
    r.wall(0, 2, 18, w=2)                       # the reactor joins at rows 18-25
    r.back(0, 2, 40, 26)
    r.stamp(16, 4, "O")
    r.stamp(22, 4, "O")
    r.stamp(27, 24, "C")
    r.girder(30, 18, 6)
    r.ent("npc", 12, 25, a="@s6_alien")
    r.ent("mirror", 20, 25)
    r.ent("reel", 33, 17, a="@s6_reel_b")
    r.ent("coin", 6, 25)
    out.append(r.done())

    r = Draft("s6_bridge", 6, (340, -30), (80, 30))
    r.fill(0, 0, 80, 2)
    r.back(0, 2, 80, 26)
    r.ground(26, 0, 12)
    r.ground(26, 22, 36)
    r.ground(26, 46, 56)
    r.ground(26, 66, 80)
    for x0, x1 in ((12, 22), (36, 46), (56, 66)):
        r.nails(x0, 29, x1 - x0)
    laser(r, 16, 2, 27, 40, 40)                 # over the first gap
    r.ent("plate", 25, 25, a=1)
    r.ent("sign", 23, 25, a="@s6_plate_hint")
    laser(r, 31, 2, 24, 0, 0, width=2, switch=1)   # dark only while the plate is held
    r.ent("charger", 52, 25)
    laser(r, 60, 2, 27, 30, 20, width=2)
    r.ent("flyer", 41, 8, a=8)
    r.ent("flyer", 70, 9, a=6)
    r.stamp(72, 23, "D")
    r.ent("heart", 68, 25)
    out.append(r.done())

    r = Draft("s6_throne", 6, (420, -30), (60, 30))
    r.fill(0, 0, 60, 2)
    r.ground(26)
    r.wall(58, 2, 26, w=2)
    r.back(0, 2, 58, 26)
    r.girder(8, 18, 8)
    r.girder(44, 18, 8)
    r.stamp(4, 24, "C")
    r.stamp(50, 24, "C")
    r.stamp(28, 5, "O")
    r.ent("boss", 40, 25, a=7)
    out.append(r.done())
    return out


def searchlight(r, x, y, sweep, beam, speed=96, phase=0):
    """A searchlight whose lamp hangs in tile (x, y). Its spot sweeps `sweep`
    tiles either side of the lamp, `beam` tiles below it, at `speed` Q8 per
    tick, starting `phase` tiles from the left end. Whoever the cone finds,
    Ruby or one of her takes, a rooftop rifle fires on."""
    r.ent("searchlight", x, y, a=sweep, b=beam, c=speed, d=phase)


def stage7():
    """Stage 7, "Night City": rain, rooftops and fire escapes, searchlights
    whose snipers fire at whoever the light finds (a take is a fine decoy),
    a jazz club, and the Fixer in his penthouse."""
    out = []
    r = Draft("s7_gate", 7, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.back(20, 6, 40, 26)                       # a hotel front
    for x in (22, 26, 30, 34):
        r.stamp(x, 8, "W" if x % 8 == 6 else "w")
        r.stamp(x, 14, "w" if x % 8 == 6 else "W")
    r.stamp(24, 2, "N")
    r.stamp(30, 20, "A")
    r.stamp(15, 21, "L")
    r.ent("door", 3, 25, a="#backlot_east", b=41, c=25)
    r.ent("mirror", 8, 25)
    r.ent("sign", 11, 25, a="@s7_title")
    r.ent("npc", 24, 25, a="@s7_newsboy")
    r.spawn(6, 25)
    out.append(r.done())

    r = Draft("s7_alley", 7, (40, 0), (40, 30))
    r.ground(26)
    r.wall(38, 8, 26, w=2)                      # a dead end at street level; the way on is up
    r.back(0, 4, 38, 26)
    r.plank(26, 21, 8)                          # fire escapes
    r.plank(30, 15, 8)
    r.plank(24, 10, 6)
    for x, y in ((4, 8), (10, 8), (4, 15), (10, 15)):
        r.stamp(x, y, "w")
    r.stamp(8, 21, "L")
    r.ent("door", 18, 25, a="#s7_speakeasy", b=5, c=17)
    r.ent("walker", 12, 25)
    r.ent("walker", 30, 25)
    r.ent("hopper", 33, 14)
    r.ent("flyer", 18, 9, a=8)
    r.ent("coin", 27, 20)
    r.ent("coin", 31, 14)
    out.append(r.done())

    r = Draft("s7_speakeasy", 7, (0, 40), (40, 20))
    r.fill(0, 0, 40, 2)
    r.ground(18)
    r.wall(0, 2, 18, w=2)
    r.wall(38, 2, 18, w=2)
    r.back(2, 2, 38, 18)
    r.stamp(10, 4, "N")
    r.stamp(28, 6, "A")
    r.ent("door", 4, 17, a="#s7_alley", b=19, c=25)
    r.ent("npc", 14, 17, a="@s7_bartender")
    r.ent("reel", 34, 17, a="@s7_reel_b")
    r.girder(22, 12, 6)
    r.ent("frame", 25, 11)
    r.ent("coin", 20, 17)
    r.ent("coin", 30, 17)
    out.append(r.done())

    r = Draft("s7_roofs_a", 7, (80, 0), (80, 30))
    r.fill(0, 8, 14, 30)
    r.fill(20, 12, 34, 30)
    r.fill(40, 9, 52, 30)
    r.fill(58, 11, 80, 30)
    for x0, x1 in ((14, 20), (34, 40), (52, 58)):
        r.nails(x0, 29, x1 - x0)
    r.stamp(44, 4, "T")
    r.stamp(24, 9, "H")
    r.stamp(66, 8, "H")
    r.stamp(4, 2, "B")
    searchlight(r, 48, 2, sweep=22, beam=10, speed=110)
    r.ent("walker", 27, 11)
    r.ent("thrower", 70, 10)
    r.ent("flyer", 37, 5, a=6)
    r.ent("coin", 17, 6)
    r.ent("coin", 37, 6)
    r.ent("coin", 55, 7)
    out.append(r.done())

    r = Draft("s7_billboard", 7, (160, -20), (40, 50))
    r.fill(0, 31, 12, 50)                       # the roof Ruby arrives on
    r.fill(26, 8, 40, 50)                       # a tall office block
    r.nails(12, 49, 14)
    r.stamp(28, 13, "B")
    for x, y, ch in ((29, 24, "W"), (34, 24, "w"), (29, 32, "w"), (34, 32, "W"), (29, 40, "W")):
        r.stamp(x, y, ch)
    r.ent("platform", 16, 10, a=21, b=1, c=90, d=3)   # a window washer's scaffold
    r.girder(12, 20, 3)
    r.ent("flyer", 20, 16, a=8)
    r.ent("coin", 17, 8)
    r.ent("coin", 19, 8)
    out.append(r.done())

    r = Draft("s7_club", 7, (200, -30), (40, 30))
    r.fill(0, 0, 40, 2)
    r.fill(0, 18, 40, 30)                       # the Blue Gardenia, on the top floor
    r.wall(0, 2, 12, w=2)
    r.back(0, 2, 40, 18)
    r.stamp(8, 4, "N")
    r.stamp(28, 4, "N")
    r.stamp(17, 7, "A")
    r.ent("mirror", 6, 17)
    r.ent("npc", 20, 17, a="@s7_singer")
    r.ent("reel", 34, 17, a="@s7_reel_a")
    r.ent("heart", 14, 17)
    out.append(r.done())

    r = Draft("s7_roofs_b", 7, (240, -30), (80, 30))
    r.fill(0, 18, 28, 30)
    r.fill(52, 18, 80, 30)
    r.nails(28, 29, 24)
    r.girder(36, 2, 8)
    searchlight(r, 40, 3, sweep=16, beam=14, speed=96)
    r.ent("platform", 29, 18, a=18, b=0, c=100, d=3)   # a slow scaffold across the gap
    r.ent("sign", 22, 17, a="@s7_decoy_hint")
    r.stamp(6, 15, "H")
    r.stamp(62, 13, "T")
    r.ent("walker", 12, 17)
    r.ent("charger", 68, 17)
    r.ent("flyer", 46, 8, a=8)
    r.ent("coin", 34, 15)
    r.ent("coin", 44, 15)
    out.append(r.done())

    r = Draft("s7_tower", 7, (320, -60), (40, 60))
    r.fill(0, 0, 40, 2)
    r.clear(30, 0, 36, 2)                       # up into the penthouse
    r.fill(0, 48, 40, 60)
    r.wall(0, 2, 40, w=2)                       # roofs_b joins at rows 40-47
    r.wall(38, 2, 48, w=2)
    r.back(0, 2, 40, 48)
    for x, y, w in ((4, 42, 8), (22, 36, 8), (6, 30, 8), (24, 24, 6), (8, 18, 8), (26, 12, 8)):
        r.girder(x, y, w)
    r.plank(30, 6, 6)
    searchlight(r, 20, 3, sweep=14, beam=27, speed=120)
    r.ent("thrower", 27, 23)
    r.ent("flyer", 14, 26, a=6)
    r.ent("flyer", 30, 15, a=6)
    r.ent("heart", 34, 47)
    for x, y in ((8, 29), (10, 29), (28, 11), (30, 11)):
        r.ent("coin", x, y)
    out.append(r.done())

    r = Draft("s7_penthouse", 7, (320, -90), (60, 30))
    r.fill(0, 0, 60, 2)
    r.ground(26)
    r.clear(30, 26, 36, 30)
    r.plank(30, 26, 6)                          # the hatch up from the tower
    r.wall(0, 2, 26, w=2)
    r.wall(58, 2, 26, w=2)
    r.back(2, 2, 58, 26)
    for x in (6, 14, 44, 52):
        r.stamp(x, 6, "W")
    r.stamp(27, 3, "N")
    r.girder(8, 19, 6)
    r.girder(46, 19, 6)
    r.ent("boss", 48, 25, a=8)
    out.append(r.done())
    return out


def beat(r, x, y, width, group):
    """A gold set piece `width` tiles wide that is solid only on alternate
    beats: group 0 on even beats, group 1 on odd ones."""
    r.ent("beat", x, y, a=width, b=group)


def stage8():
    """Stage 8, "Starlight Revue": the show that never ends. Set pieces that
    come and go on the beat, falling sandbags, a paper moon over the
    orchestra pit, Vivian Lark in her dressing room, and the Maestro."""
    out = []
    r = Draft("s8_gate", 8, (0, 0), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.back(18, 6, 40, 26)
    r.stamp(26, 8, "S")
    r.stamp(15, 21, "L")
    r.ent("door", 3, 25, a="#backlot_east", b=57, c=25)
    r.ent("mirror", 8, 25)
    r.ent("sign", 11, 25, a="@s8_title")
    r.ent("npc", 22, 25, a="@s8_doorman")
    r.spawn(6, 25)
    out.append(r.done())

    r = Draft("s8_wings", 8, (40, 0), (60, 30))
    r.fill(0, 0, 60, 2)
    r.ground(26)
    r.back(0, 2, 60, 26)
    r.clear(22, 26, 34, 28)
    r.nails(22, 28, 12)                         # a sunken trap full of prop sabres
    r.stamp(2, 19, "C")
    r.stamp(54, 19, "C")
    r.ent("sign", 18, 25, a="@s8_beat_hint")
    beat(r, 24, 24, 3, 0)
    beat(r, 29, 24, 3, 1)
    r.ent("sandbag", 40, 3)
    r.ent("sandbag", 46, 3)
    r.ent("walker", 10, 25)
    r.ent("walker", 50, 25)
    r.ent("coin", 25, 23)
    r.ent("coin", 30, 23)
    out.append(r.done())

    r = Draft("s8_stage", 8, (100, 0), (80, 30))
    r.fill(0, 0, 80, 2)
    r.fill(0, 26, 30, 30)
    r.fill(6, 23, 30, 26)
    r.fill(12, 20, 30, 23)                      # steps up onto the stage
    r.fill(50, 20, 80, 30)
    r.nails(30, 29, 20)                         # the orchestra pit
    r.clear(18, 20, 22, 30)
    r.plank(18, 20, 4)                          # a trap down to the prompter's box
    r.back(0, 2, 80, 20)
    r.stamp(4, 6, "C")
    r.stamp(73, 6, "C")
    r.stamp(37, 3, "S")
    r.stamp(56, 17, "P")
    r.ent("platform", 31, 20, a=14, b=0, c=110, d=3)   # the paper moon
    beat(r, 33, 15, 3, 0)
    beat(r, 39, 12, 3, 1)
    beat(r, 45, 15, 3, 0)
    r.ent("walker", 24, 19)
    r.ent("walker", 62, 19)
    r.ent("thrower", 72, 19)
    r.ent("coin", 40, 11)
    r.ent("coin", 34, 14)
    r.ent("coin", 46, 14)
    out.append(r.done())

    r = Draft("s8_prompter", 8, (110, 30), (30, 20))
    r.fill(0, 0, 30, 2)
    r.clear(8, 0, 12, 2)
    r.ground(16)
    r.wall(0, 2, 16, w=2)
    r.wall(28, 2, 16, w=2)
    r.back(2, 2, 28, 16)
    for y in (12, 7, 3):
        r.plank(8, y, 4)
    r.ent("reel", 24, 15, a="@s8_reel_b")
    r.ent("frame", 4, 15)
    r.ent("hopper", 18, 15)
    r.ent("coin", 14, 15)
    out.append(r.done())

    r = Draft("s8_flyloft", 8, (180, -30), (40, 60))
    r.fill(0, 0, 40, 2)
    r.fill(0, 50, 40, 60)
    r.wall(0, 2, 42, w=2)                       # the stage joins at rows 42-49
    r.wall(38, 2, 18, w=2)
    r.wall(38, 26, 50, w=2)                     # the dressing rooms join at rows 18-25
    r.back(0, 2, 40, 50)
    r.girder(4, 44, 8)
    r.girder(20, 39, 8)
    beat(r, 30, 34, 4, 0)
    beat(r, 22, 29, 4, 1)
    r.girder(8, 25, 8)
    r.ent("platform", 16, 25, a=12, b=0, c=110, d=3)
    r.girder(30, 26, 8)
    r.stamp(25, 5, "M")
    r.ent("flyer", 26, 19, a=6)
    r.ent("flyer", 10, 36, a=6)
    r.ent("coin", 10, 24)
    r.ent("coin", 12, 24)
    out.append(r.done())

    r = Draft("s8_dressing", 8, (220, -30), (40, 30))
    r.fill(0, 0, 40, 2)
    r.fill(0, 26, 40, 30)
    r.wall(0, 2, 18, w=2)
    r.wall(38, 2, 18, w=2)
    r.back(0, 2, 40, 26)
    r.stamp(4, 5, "S")
    r.stamp(28, 23, "P")
    r.ent("mirror", 8, 25)
    r.ent("npc", 16, 25, a="@s8_viv")
    r.ent("reel", 23, 25, a="@s8_reel_a")
    r.ent("heart", 35, 25)
    out.append(r.done())

    r = Draft("s8_catwalk", 8, (260, -30), (80, 30))
    r.fill(0, 0, 80, 2)
    r.girder(0, 26, 12)
    r.plank(16, 24, 8)
    beat(r, 27, 22, 3, 0)
    beat(r, 33, 22, 3, 1)
    beat(r, 39, 22, 3, 0)
    r.plank(45, 22, 10)
    r.ent("platform", 57, 22, a=10, b=0, c=120, d=3)
    r.girder(70, 26, 10)
    r.nails(0, 29, 80)
    r.stamp(20, 4, "S")
    r.stamp(52, 4, "S")
    r.ent("charger", 51, 21)
    r.ent("flyer", 30, 12, a=8)
    r.ent("flyer", 64, 14, a=8)
    for x, y in ((28, 21), (34, 21), (40, 21), (60, 20)):
        r.ent("coin", x, y)
    out.append(r.done())

    r = Draft("s8_grand", 8, (340, -30), (60, 30))
    r.fill(0, 0, 60, 2)
    r.fill(0, 26, 60, 30)
    for i in range(8):
        r.fill(10 + i * 5, 26 - i * 2, 60, 26)  # the grand staircase
    r.stamp(2, 19, "C")
    r.stamp(4, 6, "S")
    r.ent("walker", 21, 23)
    r.ent("walker", 36, 17)
    r.ent("thrower", 50, 11)
    r.ent("mirror", 56, 11)
    r.ent("coin", 31, 18)
    r.ent("coin", 41, 14)
    out.append(r.done())

    r = Draft("s8_finale", 8, (400, -44), (60, 30))
    r.fill(0, 0, 60, 2)
    r.ground(26)
    r.wall(58, 2, 26, w=2)
    r.back(0, 2, 58, 26)
    r.stamp(4, 19, "C")
    r.stamp(50, 19, "C")
    r.stamp(27, 4, "S")
    r.girder(8, 19, 6)
    r.girder(46, 19, 6)
    beat(r, 22, 20, 4, 0)
    beat(r, 34, 20, 4, 1)
    r.ent("boss", 30, 25, a=9)
    out.append(r.done())
    return out


def tower():
    """The Projection Tower (region 0, east of the lot in world space)."""
    out = []
    r = Draft("tower_base", 0, (200, 60), (40, 30))
    r.ground(26)
    r.wall(0, 0, 26, w=2)
    r.wall(38, 0, 26, w=2)
    r.fill(0, 0, 40, 2)
    r.clear(18, 0, 22, 2)
    r.back(2, 2, 38, 26)
    r.stamp(6, 8, "W")
    r.stamp(30, 8, "W")
    r.plank(14, 21, 5)
    r.plank(20, 16, 5)
    r.plank(14, 11, 5)
    r.plank(18, 6, 6)
    r.plank(18, 3, 4)
    r.ent("door", 3, 25, a="#backlot_east", b=70, c=25)
    r.ent("mirror", 7, 25)
    r.ent("sign", 11, 25, a="@tower_note")
    r.ent("heart", 30, 25)
    r.spawn(5, 25)
    out.append(r.done())

    r = Draft("tower_climb", 0, (200, 0), (40, 60))
    r.wall(0, 0, 60, w=2)
    r.wall(38, 0, 60, w=2)
    r.fill(2, 58, 18, 60)
    r.fill(22, 58, 38, 60)
    r.plank(16, 56, 8)
    r.back(2, 0, 38, 58)
    r.girder(4, 50, 8)
    r.girder(26, 45, 8)
    r.ent("platform", 14, 34, a=8, b=1, c=100, d=4)
    r.girder(4, 30, 6)
    r.ent("plate", 6, 29, a=5)
    r.plank(26, 30, 8)
    r.fill(2, 24, 38, 26)                       # the booth floor's underside...
    r.clear(30, 24, 32, 26)
    r.ent("gate", 30, 24, a=5, b=2)             # ...opened only while the mark is held
    r.ent("gate", 31, 24, a=5, b=2)
    r.girder(8, 18, 8)
    r.girder(24, 12, 8)
    r.plank(14, 7, 8)
    r.plank(16, 3, 8)
    r.ent("flyer", 12, 40, a=6)
    r.ent("flyer", 28, 16, a=6)
    r.ent("coin", 9, 17)
    r.ent("coin", 11, 17)
    out.append(r.done())

    r = Draft("tower_booth", 0, (180, -30), (80, 30))
    r.fill(0, 26, 80, 30)
    r.clear(36, 26, 44, 30)
    r.plank(36, 26, 8)                          # the hatch up from the stairs
    r.wall(0, 0, 26, w=2)
    r.wall(78, 0, 26, w=2)
    r.fill(0, 0, 80, 2)
    r.back(2, 2, 78, 26)
    r.stamp(8, 6, "W")
    r.stamp(20, 6, "w")
    r.stamp(48, 6, "W")
    r.girder(10, 18, 8)
    r.girder(24, 14, 8)
    r.ent("boss", 66, 25, a=4)
    out.append(r.done())
    return out


def all_rooms():
    return backlot() + stage1() + stage2() + stage3() + stage4() + stage5() + stage6() + stage7() + stage8() + tower()
