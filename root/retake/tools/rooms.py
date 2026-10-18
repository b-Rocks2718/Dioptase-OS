"""ASCII room format, autotiling and composition for RETAKE.

A room file holds one or more rooms:

    room <name>
    region <n>
    pos <wx> <wy>          world position of the top-left tile (in tiles)
    size <w> <h>           in 8-pixel tiles, at most 80x60
    map                    h rows of w collision/material characters
    ...
    deco                   optional: h rows of w background characters
    ...
    entities               optional: one "<type> <tx> <ty> [key=value ...]" per line
    ...
    end

Map characters (collision codes in parentheses, see COL_* in the engine):
    .  air (0)          #  brick solid (1)     =  girder solid (1)
    -  plank one-way (2)
    ^ v < >  nails pointing up/down/left/right (3)
    S  hero spawn (air)

Deco characters (painted behind the foreground; ' ' and '.' are open sky):
    b  back-wall facade      W w  lit/dark window   D  door    L  street lamp
    C  clock face            H K F  HOTEL/BANK/CAFE signs
A stamp character paints its stamp with the tile's top-left as anchor; the
cell itself takes the fill of its left neighbor.

The composer bakes the whole room into one image (backgrounds, stamps, drop
shadows, foreground tiles); build.py then splits it into 8x8 blocks shared
across a region.
"""
import numpy as np

import pix
from pix import TRANSPARENT

T = 8
MAX_W, MAX_H = 80, 60

COL_AIR, COL_SOLID, COL_ONEWAY, COL_SPIKE = 0, 1, 2, 3

MAP_CODES = {
    ".": COL_AIR, "S": COL_AIR,
    "#": COL_SOLID, "=": COL_SOLID,
    "-": COL_ONEWAY,
    "^": COL_SPIKE, "v": COL_SPIKE, "<": COL_SPIKE, ">": COL_SPIKE,
}

STAMP_CHARS = {
    "W": "window_lit", "w": "window_dark", "D": "door", "L": "lamp",
    "C": "clock", "H": "sign_hotel", "K": "sign_bank", "F": "sign_cafe",
}


class Room:
    def __init__(self, name):
        self.name = name
        self.region = 1
        self.wx = self.wy = 0
        self.w = self.h = 0
        self.map = []
        self.deco = []
        self.entities = []  # (type, tx, ty, {key: value})
        self.source = ""

    def col(self):
        """Collision grid as a list of rows of COL_* codes."""
        return [[MAP_CODES[ch] for ch in row] for row in self.map]

    def spawn(self):
        for y, row in enumerate(self.map):
            x = row.find("S")
            if x >= 0:
                return x, y
        return None


def parse(path):
    rooms = []
    cur = None
    section = None
    with open(path) as f:
        lines = f.read().split("\n")
    for lineno, raw in enumerate(lines, 1):
        line = raw.rstrip()
        where = f"{path}:{lineno}"
        if section in ("map", "deco") and line and not line.startswith(("deco", "entities", "end", "map")):
            (cur.map if section == "map" else cur.deco).append(line)
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        words = stripped.split()
        key = words[0]
        if key == "room":
            cur = Room(words[1])
            cur.source = where
            section = None
        elif cur is None:
            raise ValueError(f"{where}: expected 'room <name>' before '{key}'")
        elif key == "region":
            cur.region = int(words[1])
        elif key == "pos":
            cur.wx, cur.wy = int(words[1]), int(words[2])
        elif key == "size":
            cur.w, cur.h = int(words[1]), int(words[2])
        elif key in ("map", "deco", "entities"):
            section = key
        elif key == "end":
            validate(cur)
            rooms.append(cur)
            cur, section = None, None
        elif section == "entities":
            props = {}
            for w in words[3:]:
                k, _, v = w.partition("=")
                props[k] = v
            cur.entities.append((words[0], int(words[1]), int(words[2]), props))
        else:
            raise ValueError(f"{where}: unexpected line '{stripped}' (section {section})")
    if cur is not None:
        raise ValueError(f"{path}: room '{cur.name}' is missing its 'end' line")
    return rooms


def validate(room):
    where = f"room '{room.name}' ({room.source})"
    if not (1 <= room.w <= MAX_W and 1 <= room.h <= MAX_H):
        raise ValueError(f"{where}: size {room.w}x{room.h} must be within 1..{MAX_W} x 1..{MAX_H} tiles")
    if len(room.map) != room.h:
        raise ValueError(f"{where}: map has {len(room.map)} rows, expected {room.h}")
    for i, row in enumerate(room.map):
        if len(row) != room.w:
            raise ValueError(f"{where}: map row {i} has {len(row)} columns, expected {room.w}")
        for ch in row:
            if ch not in MAP_CODES:
                raise ValueError(f"{where}: map row {i} has unknown character {ch!r}")
    if room.deco:
        if len(room.deco) != room.h:
            raise ValueError(f"{where}: deco has {len(room.deco)} rows, expected {room.h}")
        for i, row in enumerate(room.deco):
            if len(row) != room.w:
                raise ValueError(f"{where}: deco row {i} has {len(row)} columns, expected {room.w}")
    else:
        room.deco = ["." * room.w for _ in range(room.h)]


def _solid_mask(room, x, y, chars):
    """4-bit neighbor mask (1 up, 2 right, 4 down, 8 left) of same-material
    cells; outside the room counts as solid so edges render closed."""
    m = 0
    for bit, (dx, dy) in ((1, (0, -1)), (2, (1, 0)), (4, (0, 1)), (8, (-1, 0))):
        nx, ny = x + dx, y + dy
        if nx < 0 or ny < 0 or nx >= room.w or ny >= room.h or room.map[ny][nx] in chars:
            m |= bit
    return m


def compose(room, tiles, stamps, stamp_chars=None):
    """Bake a room into a (h*8, w*8) packed image; open sky is TRANSPARENT.
    `stamp_chars` maps deco characters to stamp names (default STAMP_CHARS)."""
    chars = stamp_chars if stamp_chars is not None else STAMP_CHARS
    W, H = room.w * T, room.h * T
    img = pix.new_image(W, H)
    # 1. back-wall fill (stamp cells inherit their left neighbor's fill)
    fill = [[False] * room.w for _ in range(room.h)]
    for y in range(room.h):
        for x in range(room.w):
            ch = room.deco[y][x]
            if ch == "b":
                fill[y][x] = True
            elif ch in chars and x > 0:
                fill[y][x] = fill[y][x - 1]
    for y in range(room.h):
        for x in range(room.w):
            if fill[y][x]:
                v = (x * 7 + y * 13) % 4
                img[y * T:(y + 1) * T, x * T:(x + 1) * T] = tiles[f"facade_{v}"]
    # facade edges: a lit cornice line on top, dark lines on the sides
    for y in range(room.h):
        for x in range(room.w):
            if not fill[y][x]:
                continue
            px, py = x * T, y * T
            if y == 0 or not fill[y - 1][x]:
                img[py, px:px + T] = pix.gray(9, 1)
                img[py + 1, px:px + T] = pix.gray(7, 1)
                img[py + 2, px:px + T] = pix.gray(2, 1)
            if x == 0 or not fill[y][x - 1]:
                img[py:py + T, px] = pix.gray(2, 1)
            if x == room.w - 1 or not fill[y][x + 1]:
                img[py:py + T, px + T - 1] = pix.gray(2, 1)
    # 2. stamps
    for y in range(room.h):
        for x in range(room.w):
            name = chars.get(room.deco[y][x])
            if name:
                pix.blit(img, stamps[name], x * T, y * T)
    # 3. drop shadows of foreground solids onto the background (down-right)
    solid = np.zeros((H, W), dtype=bool)
    for y in range(room.h):
        for x in range(room.w):
            if room.map[y][x] in "#=":
                solid[y * T:(y + 1) * T, x * T:(x + 1) * T] = True
    shadow = np.zeros_like(solid)
    shadow[3:, 2:] = solid[:-3, :-2]
    shadow &= ~solid
    bg = img != TRANSPARENT
    darken = shadow & bg
    if darken.any():
        vals = img[darken]
        r = np.maximum((vals & 15).astype(np.int32) - 3, 0)
        gg = np.maximum(((vals >> 4) & 15).astype(np.int32) - 3, 0)
        b = np.maximum(((vals >> 8) & 15).astype(np.int32) - 3, 0)
        img[darken] = (b << 8) | (gg << 4) | r
    # 4. foreground tiles
    for y in range(room.h):
        for x in range(room.w):
            ch = room.map[y][x]
            t = None
            if ch == "#":
                t = tiles[f"brick_{_solid_mask(room, x, y, '#=')}_{(x * 3 + y * 5) % 2}"]
            elif ch == "=":
                t = tiles[f"girder_{_solid_mask(room, x, y, '=')}"]
            elif ch == "-":
                t = tiles[f"plank_{(x + y) % 2}"]
            elif ch == "^":
                t = tiles["nails_up"]
            elif ch == "v":
                t = tiles["nails_down"]
            elif ch == "<":
                t = tiles["nails_left"]
            elif ch == ">":
                t = tiles["nails_right"]
            if t is not None:
                pix.blit(img, t, x * T, y * T)
    return img
