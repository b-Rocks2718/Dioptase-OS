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
        r.ent("sign", x + 7, 25, a="@stage_sealed")
    r.stamp(68, 11, "X")
    r.ent("door", 71, 25, a="#tower_base", b=5, c=25, d=1 | (3 << 4))
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
    return backlot() + stage1() + stage2() + stage3() + tower()
