"""A small Python DSL for authoring rooms; it produces rooms.Room objects
(the same structure the ASCII room files parse into), so build.py treats
both alike. Coordinates are tiles; rectangles are [x0, x1) x [y0, y1).

    r = Draft("s1_street", region=1, pos=(40, 30), size=(80, 30))
    r.ground(26)
    r.building(2, 8, 16, 26, windows="W")
    r.plank(20, 18, 6)
    r.ent("walker", 30, 25)
    r.spawn(3, 25)
    room = r.done()
"""
import rooms


class Draft:
    def __init__(self, name, region, pos, size):
        self.room = rooms.Room(name)
        self.room.region = region
        self.room.wx, self.room.wy = pos
        self.room.w, self.room.h = size
        self.room.source = f"roomdsl:{name}"
        w, h = size
        self.m = [["."] * w for _ in range(h)]
        self.d = [["."] * w for _ in range(h)]

    # -- collision layer
    def fill(self, x0, y0, x1, y1, ch="#"):
        for y in range(max(0, y0), min(self.room.h, y1)):
            for x in range(max(0, x0), min(self.room.w, x1)):
                self.m[y][x] = ch
        return self

    def clear(self, x0, y0, x1, y1):
        return self.fill(x0, y0, x1, y1, ".")

    def ground(self, y, x0=0, x1=None, ch="#"):
        return self.fill(x0, y, self.room.w if x1 is None else x1, self.room.h, ch)

    def ceiling(self, y, x0=0, x1=None, ch="#"):
        return self.fill(x0, 0, self.room.w if x1 is None else x1, y, ch)

    def wall(self, x, y0=0, y1=None, w=1, ch="#"):
        return self.fill(x, y0, x + w, self.room.h if y1 is None else y1, ch)

    def plank(self, x, y, w):
        return self.fill(x, y, x + w, y + 1, "-")

    def girder(self, x, y, w, h=1):
        return self.fill(x, y, x + w, y + h, "=")

    def block(self, x, y, w, h):
        return self.fill(x, y, x + w, y + h, "#")

    def nails(self, x, y, w, d="^"):
        return self.fill(x, y, x + w, y + 1, d)

    def spawn(self, x, y):
        self.m[y][x] = "S"
        return self

    # -- background layer
    def back(self, x0, y0, x1, y1, ch="b"):
        for y in range(max(0, y0), min(self.room.h, y1)):
            for x in range(max(0, x0), min(self.room.w, x1)):
                self.d[y][x] = ch
        return self

    def stamp(self, x, y, ch):
        self.d[y][x] = ch
        return self

    def building(self, x0, y0, x1, y1, windows="W", step=5, rows=5, door=None, sign=None):
        """A back-wall facade with a grid of windows (alternating lit/dark
        when windows is "Ww")."""
        self.back(x0, y0, x1, y1)
        wy = y0 + 2
        k = 0
        while wy + 3 < y1 - (4 if door else 1):
            wx = x0 + 2
            while wx + 2 <= x1 - 1:
                covered = False
                if sign:
                    sx, sy, _ = sign
                    # a window is 2x3 tiles, a sign about 5x2
                    covered = wx < sx + 5 and sx < wx + 2 and wy < sy + 2 and sy < wy + 3
                if not covered:
                    self.stamp(wx, wy, windows[k % len(windows)])
                k += 1
                wx += step
            wy += rows
        if door:
            self.stamp(door, y1 - 4, "D")
        if sign:
            sx, sy, ch = sign
            self.stamp(sx, sy, ch)
        return self

    # -- entities
    def ent(self, kind, x, y, **params):
        self.room.entities.append((kind, x, y, {k: str(v) for k, v in params.items()}))
        return self

    def done(self):
        self.room.map = ["".join(row) for row in self.m]
        self.room.deco = ["".join(row) for row in self.d]
        rooms.validate(self.room)
        return self.room

    def text(self):
        """The room in the ASCII file format (for inspection or export)."""
        r = self.done()
        out = [f"room {r.name}", f"region {r.region}", f"pos {r.wx} {r.wy}", f"size {r.w} {r.h}", "map"]
        out += r.map + ["deco"] + r.deco + ["entities"]
        for kind, x, y, props in r.entities:
            out.append(" ".join([kind, str(x), str(y)] + [f"{k}={v}" for k, v in props.items()]))
        out.append("end")
        return "\n".join(out) + "\n"
