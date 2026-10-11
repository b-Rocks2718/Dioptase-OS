#!/usr/bin/env python3
"""Generate DELVE's art assets as Dioptase assembler data (assets.s).

Everything is procedural: a 256-color palette made of 16 ramps x 16 shades,
the per-light-level color tables the renderer uses for shading, wall/floor
textures, billboard sprites, the HUD font, and the hardware-sprite overlays.

Texture and sprite pixels are palette indices (ramp * 16 + shade). Index 0 is
black in walls and "transparent" in billboards. The renderer converts indices
to 12-bit 0x0BGR VGA colors through `shade_tables` (docs/mem_map.md).

Usage: gen_assets.py <out.s> [--preview <dir>]
"""
import sys
import os

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngout import write_png  # noqa: E402

RNG = np.random.default_rng(0xD10D)
TEX = 64  # wall/floor texture size in texels

# ---------------------------------------------------------------------------
# Palette: 16 ramps, each a dark->light gradient through three key colors.
# ---------------------------------------------------------------------------
GRAY, STONE, WOOD, RUST, MOSS, GEM, ICE, BLUE, PURPLE, RED, FIRE, GOLD, BONE, SLIME, PINK, DEEP = range(16)

RAMP_KEYS = [
    [(0, 0, 0), (120, 120, 120), (255, 255, 255)],       # GRAY
    [(14, 12, 12), (104, 96, 88), (215, 205, 190)],      # STONE
    [(22, 12, 5), (120, 72, 34), (230, 180, 120)],       # WOOD
    [(20, 9, 6), (130, 58, 34), (240, 150, 100)],        # RUST
    [(6, 16, 4), (56, 100, 34), (180, 220, 120)],        # MOSS
    [(0, 22, 14), (0, 170, 110), (180, 255, 225)],       # GEM (dioptase green)
    [(0, 12, 24), (50, 140, 180), (210, 250, 255)],      # ICE
    [(6, 6, 30), (50, 70, 200), (180, 200, 255)],        # BLUE
    [(14, 4, 24), (120, 50, 170), (235, 180, 255)],      # PURPLE
    [(24, 2, 2), (185, 25, 20), (255, 170, 150)],        # RED
    [(30, 8, 0), (230, 100, 10), (255, 240, 140)],       # FIRE
    [(24, 16, 0), (210, 165, 25), (255, 252, 190)],      # GOLD
    [(24, 18, 12), (180, 150, 118), (255, 245, 225)],    # BONE
    [(8, 20, 0), (100, 190, 25), (225, 255, 150)],       # SLIME
    [(24, 0, 14), (210, 45, 150), (255, 200, 235)],      # PINK
    [(3, 4, 8), (44, 52, 70), (160, 175, 200)],          # DEEP
]


def build_palette():
    pal = np.zeros((256, 3), dtype=np.float64)
    for r, keys in enumerate(RAMP_KEYS):
        keys = np.array(keys, dtype=np.float64)
        for s in range(16):
            t = s / 15.0
            if t < 0.5:
                c = keys[0] + (keys[1] - keys[0]) * (t / 0.5)
            else:
                c = keys[1] + (keys[2] - keys[1]) * ((t - 0.5) / 0.5)
            pal[r * 16 + s] = c
    return pal


PALETTE = build_palette()

# Light levels: LIGHT_FULL is unshaded; above it colors are overbright (used
# for spell flashes); below it they fade toward the dungeon fog color.
LIGHT_LEVELS = 16
LIGHT_FULL = 11
FOG = np.array([2.0, 3.0, 8.0])


def light_factor(level):
    if level <= LIGHT_FULL:
        return (level / LIGHT_FULL) ** 1.15
    return 1.0 + (level - LIGHT_FULL) * 0.16


def to12(rgb):
    """Quantize an (..., 3) 0-255 array to 4-bit channels."""
    return np.clip(np.round(np.asarray(rgb) / 17.0), 0, 15).astype(np.int64)


def pack12(c4):
    """Pack 4-bit r, g, b into the VGA's 0x0BGR halfword."""
    return int(c4[0]) | (int(c4[1]) << 4) | (int(c4[2]) << 8)


def build_shade_tables():
    tables = np.zeros((LIGHT_LEVELS, 256), dtype=np.int64)
    for level in range(LIGHT_LEVELS):
        f = light_factor(level)
        for i in range(256):
            if f <= 1.0:
                c = PALETTE[i] * f + FOG * (1.0 - f)
            else:
                c = PALETTE[i] * f + (f - 1.0) * 40.0
            tables[level, i] = pack12(to12(c))
    return tables


def idx(ramp, shade):
    shade = np.clip(np.round(shade), 0, 15).astype(np.int64)
    return ramp * 16 + shade


# ---------------------------------------------------------------------------
# Noise helpers (tileable value noise on a TEX x TEX texture).
# ---------------------------------------------------------------------------
def value_noise(cells, size=TEX, rng=RNG):
    """Tileable smooth value noise in [0, 1] with `cells` lattice cells."""
    lat = rng.random((cells, cells))
    coords = np.arange(size) * cells / size
    i0 = np.floor(coords).astype(int)
    f = coords - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    i0 %= cells
    a = lat[np.ix_(i0, i0)]
    b = lat[np.ix_(i0, i1)]
    c = lat[np.ix_(i1, i0)]
    d = lat[np.ix_(i1, i1)]
    fy = f[:, None]
    fx = f[None, :]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


def fbm(octaves=4, base=4, rng=RNG, size=TEX):
    total = np.zeros((size, size))
    amp = 1.0
    norm = 0.0
    cells = base
    for _ in range(octaves):
        total += value_noise(cells, size, rng) * amp
        norm += amp
        amp *= 0.5
        cells *= 2
        if cells > size:
            cells = size
    return total / norm


# ---------------------------------------------------------------------------
# Wall textures. Each returns a (TEX, TEX) array of palette indices in
# row-major [y][x] order; emit() stores them column-major for the renderer.
# ---------------------------------------------------------------------------
def bricks(ramp=STONE, rows=8, cols=4, mortar_shade=2.0, base=7.0, moss=0.0, rng=RNG):
    h = TEX // rows
    w = TEX // cols
    noise = fbm(4, 8, rng)
    grain = rng.random((TEX, TEX))
    shade = np.zeros((TEX, TEX))
    ramps = np.full((TEX, TEX), ramp)
    for by in range(rows):
        offset = (w // 2) * (by % 2)
        for bx in range(cols + 1):
            tone = base + rng.normal(0, 1.1)
            for y in range(by * h, by * h + h):
                for xx in range(bx * w - offset, bx * w - offset + w):
                    x = xx % TEX
                    ly = y - by * h
                    lx = xx - (bx * w - offset)
                    if ly == h - 1 or lx == w - 1:
                        shade[y, x] = mortar_shade + grain[y, x]
                        continue
                    s = tone + (noise[y, x] - 0.5) * 4.0 + (grain[y, x] - 0.5) * 1.2
                    if ly == 0 or lx == 0:
                        s += 1.6  # bevel catches the light
                    elif ly == h - 2 or lx == w - 2:
                        s -= 1.4  # shadowed lower/right edge
                    shade[y, x] = s
    if moss > 0:
        m = fbm(4, 4, rng)
        ys = np.arange(TEX)[:, None] / TEX
        mask = (m + ys * 0.55) > (1.25 - moss)
        ramps = np.where(mask, MOSS, ramps)
        shade = np.where(mask, shade * 0.85 + 1.0, shade)
    # a few cracks
    for _ in range(3):
        x, y = rng.integers(0, TEX, 2)
        for _ in range(rng.integers(6, 14)):
            shade[y % TEX, x % TEX] = 1.5
            x += rng.integers(-1, 2)
            y += 1
    return idx(ramps, np.clip(shade, 1, 14))


def cave_rock(ramp=STONE, rng=RNG, crystals=0):
    n = fbm(5, 4, rng)
    ridges = 1.0 - np.abs(fbm(4, 8, rng) * 2 - 1)
    shade = 2.5 + n * 9.0 - (ridges > 0.86) * 3.0
    ramps = np.full((TEX, TEX), ramp)
    out = idx(ramps, np.clip(shade, 1, 13))
    for _ in range(crystals):
        cx, cy = rng.integers(6, TEX - 6, 2)
        size = rng.integers(3, 7)
        draw_crystal(out, cx, cy, size, rng)
    return out


def draw_crystal(img, cx, cy, size, rng):
    """Draw a faceted hexagonal dioptase crystal (lit from the upper left)."""
    angle = rng.uniform(-0.6, 0.6)
    ca, sa = np.cos(angle), np.sin(angle)
    for y in range(cy - size * 2, cy + size * 2 + 1):
        for x in range(cx - size * 2, cx + size * 2 + 1):
            dx, dy = x - cx, y - cy
            u = dx * ca + dy * sa
            v = -dx * sa + dy * ca
            # elongated hexagon: |v| < size, |u| + |v|/2 < size*1.6
            if abs(v) <= size and abs(u) + abs(v) * 0.6 <= size * 1.7:
                facet = 0
                if u < -size * 0.6:
                    facet = 1
                elif u > size * 0.6:
                    facet = 2
                s = 9 + (-v / size) * 3 + (3 if facet == 1 else (-2 if facet == 2 else 0))
                if abs(v) >= size - 0.5 or abs(u) + abs(v) * 0.6 >= size * 1.7 - 0.8:
                    s = 4
                img[y % TEX, x % TEX] = idx(GEM, min(15, max(2, s)))


def wood_door(rng=RNG):
    shade = np.zeros((TEX, TEX))
    ramps = np.full((TEX, TEX), WOOD)
    grain = fbm(3, 4, rng)
    planks = 5
    pw = TEX / planks
    for x in range(TEX):
        p = int(x / pw)
        tone = 6.5 + ((p * 7) % 3 - 1) * 0.7
        lx = x - p * pw
        for y in range(TEX):
            g = np.sin((y * 0.35 + grain[y, x] * 9 + p * 3.1)) * 0.9
            s = tone + g + (grain[y, x] - 0.5) * 2
            if lx < 1:
                s = 2.5
            elif lx < 2:
                s += 1.2
            shade[y, x] = s
    # frame
    for i in range(TEX):
        for t in range(3):
            for (y, x) in ((t, i), (TEX - 1 - t, i), (i, t), (i, TEX - 1 - t)):
                ramps[y, x] = STONE
                shade[y, x] = 4 + t
    # iron bands with rivets
    for by in (12, 48):
        for y in range(by, by + 5):
            for x in range(3, TEX - 3):
                ramps[y, x] = GRAY
                shade[y, x] = 5 + (y == by) * 3 - (y == by + 4) * 2
        for x in range(8, TEX - 5, 12):
            ramps[by + 2, x] = GRAY
            shade[by + 2, x] = 12
            shade[by + 3, x] = 3
    # ring handle
    for a in np.linspace(0, 2 * np.pi, 40):
        x = int(round(46 + 4 * np.cos(a)))
        y = int(round(34 + 4 * np.sin(a)))
        ramps[y, x] = GOLD
        shade[y, x] = 9 + 3 * (np.sin(a) < 0)
    return idx(ramps, np.clip(shade, 1, 15))


def iron_door(rng=RNG):
    n = fbm(4, 8, rng)
    shade = 4.5 + n * 3
    ramps = np.full((TEX, TEX), DEEP)
    for i in range(TEX):
        for t in range(4):
            for (y, x) in ((t, i), (TEX - 1 - t, i), (i, t), (i, TEX - 1 - t)):
                ramps[y, x] = GRAY
                shade[y, x] = 6 + (t == 0) * 3
    for y in range(8, TEX - 8, 8):
        for x in range(8, TEX - 7, 8):
            ramps[y, x] = GRAY
            shade[y, x] = 12
            shade[min(y + 1, TEX - 1), x] = 2
    # gold lock plate with keyhole
    for y in range(24, 42):
        for x in range(25, 39):
            ramps[y, x] = GOLD
            edge = y in (24, 41) or x in (25, 38)
            shade[y, x] = 7 if edge else 10 - (y - 24) * 0.15
    for y in range(29, 37):
        for x in range(31, 33):
            ramps[y, x] = GRAY
            shade[y, x] = 1
    for y in range(28, 31):
        for x in range(30, 34):
            ramps[y, x] = GRAY
            shade[y, x] = 1
    return idx(ramps, np.clip(shade, 1, 15))


def rune_wall(rng=RNG, ramp=PURPLE):
    base = cave_rock(DEEP, rng)
    img = base.copy()
    # Carve a grid of glowing glyphs: random strokes on a 3x3 lattice.
    for gx in (12, 32, 52):
        for gy in (14, 34, 52):
            if rng.random() < 0.35:
                continue
            pts = [(gx + dx * 4, gy + dy * 4) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
            for _ in range(rng.integers(2, 5)):
                a = pts[rng.integers(0, 9)]
                b = pts[rng.integers(0, 9)]
                for t in np.linspace(0, 1, 12):
                    x = int(round(a[0] + (b[0] - a[0]) * t))
                    y = int(round(a[1] + (b[1] - a[1]) * t))
                    img[y, x] = idx(ramp, 13)
                    for (oy, ox) in ((0, 1), (1, 0), (0, -1), (-1, 0)):
                        yy, xx = y + oy, x + ox
                        if img[yy, xx] // 16 != ramp:
                            img[yy, xx] = idx(ramp, 6)
    return img



# ---------------------------------------------------------------------------
# Fonts
# ---------------------------------------------------------------------------
# 3x5 HUD font for ASCII 32..95. Each glyph is five rows of three pixels.
SMALL_FONT = {
    ' ': ["...", "...", "...", "...", "..."],
    '!': [".#.", ".#.", ".#.", "...", ".#."],
    '"': ["#.#", "#.#", "...", "...", "..."],
    '#': ["#.#", "###", "#.#", "###", "#.#"],
    '$': [".##", "##.", ".#.", ".##", "##."],
    '%': ["#.#", "..#", ".#.", "#..", "#.#"],
    '&': [".#.", "#.#", ".#.", "#.#", ".##"],
    "'": [".#.", ".#.", "...", "...", "..."],
    '(': ["..#", ".#.", ".#.", ".#.", "..#"],
    ')': ["#..", ".#.", ".#.", ".#.", "#.."],
    '*': ["...", "#.#", ".#.", "#.#", "..."],
    '+': ["...", ".#.", "###", ".#.", "..."],
    ',': ["...", "...", "...", ".#.", "#.."],
    '-': ["...", "...", "###", "...", "..."],
    '.': ["...", "...", "...", "...", ".#."],
    '/': ["..#", "..#", ".#.", "#..", "#.."],
    '0': ["###", "#.#", "#.#", "#.#", "###"],
    '1': [".#.", "##.", ".#.", ".#.", "###"],
    '2': ["##.", "..#", ".#.", "#..", "###"],
    '3': ["##.", "..#", ".#.", "..#", "##."],
    '4': ["#.#", "#.#", "###", "..#", "..#"],
    '5': ["###", "#..", "##.", "..#", "##."],
    '6': [".##", "#..", "###", "#.#", "###"],
    '7': ["###", "..#", ".#.", ".#.", ".#."],
    '8': ["###", "#.#", "###", "#.#", "###"],
    '9': ["###", "#.#", "###", "..#", "##."],
    ':': ["...", ".#.", "...", ".#.", "..."],
    ';': ["...", ".#.", "...", ".#.", "#.."],
    '<': ["..#", ".#.", "#..", ".#.", "..#"],
    '=': ["...", "###", "...", "###", "..."],
    '>': ["#..", ".#.", "..#", ".#.", "#.."],
    '?': ["##.", "..#", ".#.", "...", ".#."],
    '@': [".#.", "#.#", "###", "#..", ".##"],
    'A': [".#.", "#.#", "###", "#.#", "#.#"],
    'B': ["##.", "#.#", "##.", "#.#", "##."],
    'C': [".##", "#..", "#..", "#..", ".##"],
    'D': ["##.", "#.#", "#.#", "#.#", "##."],
    'E': ["###", "#..", "##.", "#..", "###"],
    'F': ["###", "#..", "##.", "#..", "#.."],
    'G': [".##", "#..", "#.#", "#.#", ".##"],
    'H': ["#.#", "#.#", "###", "#.#", "#.#"],
    'I': ["###", ".#.", ".#.", ".#.", "###"],
    'J': ["..#", "..#", "..#", "#.#", ".#."],
    'K': ["#.#", "#.#", "##.", "#.#", "#.#"],
    'L': ["#..", "#..", "#..", "#..", "###"],
    'M': ["#.#", "###", "###", "#.#", "#.#"],
    'N': ["##.", "#.#", "#.#", "#.#", "#.#"],
    'O': [".#.", "#.#", "#.#", "#.#", ".#."],
    'P': ["##.", "#.#", "##.", "#..", "#.."],
    'Q': [".#.", "#.#", "#.#", "##.", ".##"],
    'R': ["##.", "#.#", "##.", "#.#", "#.#"],
    'S': [".##", "#..", ".#.", "..#", "##."],
    'T': ["###", ".#.", ".#.", ".#.", ".#."],
    'U': ["#.#", "#.#", "#.#", "#.#", "###"],
    'V': ["#.#", "#.#", "#.#", "#.#", ".#."],
    'W': ["#.#", "#.#", "###", "###", "#.#"],
    'X': ["#.#", "#.#", ".#.", "#.#", "#.#"],
    'Y': ["#.#", "#.#", ".#.", ".#.", ".#."],
    'Z': ["###", "..#", ".#.", "#..", "###"],
    '[': [".##", ".#.", ".#.", ".#.", ".##"],
    '\\': ["#..", "#..", ".#.", "..#", "..#"],
    ']': ["##.", ".#.", ".#.", ".#.", "##."],
    '^': [".#.", "#.#", "...", "...", "..."],
    '_': ["...", "...", "...", "...", "###"],
}


def small_font_bytes():
    """Five row masks per glyph for ASCII 32..95; bit 2 is the left column."""
    data = []
    for code in range(32, 96):
        rows = SMALL_FONT.get(chr(code), SMALL_FONT['?'])
        for r in rows:
            data.append(sum(1 << (2 - i) for i, ch in enumerate(r) if ch == '#'))
    return data


def big_font_bytes(text_tiles_path):
    """Decode the OS's own 8x8 text font (common/text_tiles.s) into eight
    row masks per glyph for ASCII 32..127 (bit 7 = left column), followed by
    one (left, width) byte pair per glyph for proportional spacing."""
    import re
    pixels = {}
    with open(text_tiles_path) as f:
        for line in f:
            m = re.match(r"\s*\.fild (0x[0-9A-Fa-f]+)", line)
            if m:
                v = int(m.group(1), 16)
                pixels.setdefault(v >> 6, set()).add(v & 63)
    rows = []
    spans = []
    for code in range(32, 128):
        bits = pixels.get(code, set())
        cols = [p % 8 for p in bits]
        left = min(cols) if cols else 0
        width = (max(cols) - left + 1) if cols else 3
        for y in range(8):
            rows.append(sum(1 << (7 - x) for x in range(8) if y * 8 + x in bits))
        spans.extend([left, width])
    return rows, spans


def flagstones(ramp=STONE, rng=RNG, base=6.0):
    """Irregular square flagstones for the floor (tiles every cell)."""
    noise = fbm(4, 8, rng)
    grain = rng.random((TEX, TEX))
    shade = np.zeros((TEX, TEX))
    # four stones per cell with jittered seams
    seam_x = [0, 32 + int(rng.integers(-4, 5))]
    seam_y = [0, 32 + int(rng.integers(-4, 5))]
    tones = rng.normal(base, 0.9, (2, 2))
    for y in range(TEX):
        for x in range(TEX):
            sx = 1 if x >= seam_x[1] else 0
            sy = 1 if y >= seam_y[1] else 0
            lx = x - seam_x[sx]
            ly = y - seam_y[sy]
            w = (seam_x[1] if sx == 0 else TEX - seam_x[1])
            h = (seam_y[1] if sy == 0 else TEX - seam_y[1])
            s = tones[sy, sx] + (noise[y, x] - 0.5) * 3.5 + (grain[y, x] - 0.5) * 1.0
            edge = min(lx, ly, w - 1 - lx, h - 1 - ly)
            if edge == 0:
                s = 1.5 + grain[y, x]
            elif edge == 1:
                s -= 1.2 if (lx == w - 2 or ly == h - 2) else -1.0
            shade[y, x] = s
    for _ in range(4):
        x, y = rng.integers(0, TEX, 2)
        for _ in range(rng.integers(5, 12)):
            shade[y % TEX, x % TEX] = 2
            x += rng.integers(-1, 2)
            y += rng.integers(0, 2)
    return idx(np.full((TEX, TEX), ramp), np.clip(shade, 1, 14))


def ceiling_beams(rng=RNG):
    """Dark rock ceiling crossed by a wooden beam."""
    rock = cave_rock(DEEP, rng)
    out = rock.copy()
    grain = fbm(3, 4, rng)
    for y in range(TEX):
        for x in range(24, 40):
            s = 5.5 + np.sin(y * 0.4 + grain[y, x] * 8) * 0.8 + (grain[y, x] - 0.5) * 2
            if x in (24, 39):
                s = 2.5
            elif x == 25:
                s += 1.5
            out[y, x] = idx(WOOD, np.clip(s, 1, 12))
    return out


FLATS = []  # floor/ceiling textures, row-major


TEXTURES = []  # (name, row-major index array)


def add_texture(name, img):
    TEXTURES.append((name, np.asarray(img, dtype=np.int64)))


def build_textures():
    add_texture("brick", bricks(STONE, rng=np.random.default_rng(1)))
    add_texture("brick_moss", bricks(STONE, moss=0.55, rng=np.random.default_rng(2)))
    add_texture("rock", cave_rock(RUST, rng=np.random.default_rng(3)))
    add_texture("rock_gem", cave_rock(DEEP, rng=np.random.default_rng(4), crystals=6))
    add_texture("door", wood_door(np.random.default_rng(5)))
    add_texture("door_locked", iron_door(np.random.default_rng(6)))
    add_texture("runes", rune_wall(np.random.default_rng(7)))
    add_texture("brick_dark", bricks(DEEP, rows=8, cols=2, base=6.0, rng=np.random.default_rng(8)))
    FLATS.append(("floor_stone", flagstones(STONE, np.random.default_rng(20))))
    FLATS.append(("ceiling_beams", ceiling_beams(np.random.default_rng(21))))
    FLATS.append(("floor_moss", flagstones(MOSS, np.random.default_rng(22), base=5.0)))
    FLATS.append(("ceiling_rock", cave_rock(DEEP, np.random.default_rng(23))))


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def emit_words(out, words):
    """Emit 32-bit words, one `.fill` per line (the assembler takes one
    directive per line)."""
    for w in words:
        w = int(w) & 0xFFFFFFFF
        if w >= 0x80000000:
            w -= 1 << 32
        out.append(f"  .fill {w}")


def emit_bytes(out, label, data):
    """Emit bytes packed little-endian into words; pads to a word boundary."""
    if label:
        out.append(f"  .global {label}")
        out.append(f"{label}:")
    data = [int(v) & 0xFF for v in data]
    while len(data) % 4:
        data.append(0)
    emit_words(out, [data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) | (data[i + 3] << 24)
                     for i in range(0, len(data), 4)])


def emit_halfs(out, label, data):
    """Emit halfwords packed little-endian into words; pads to a word boundary."""
    if label:
        out.append(f"  .global {label}")
        out.append(f"{label}:")
    data = [int(v) & 0xFFFF for v in data]
    if len(data) % 2:
        data.append(0)
    emit_words(out, [data[i] | (data[i + 1] << 16) for i in range(0, len(data), 2)])


def preview(dirname, shade_tables):
    os.makedirs(dirname, exist_ok=True)
    full = shade_tables[LIGHT_FULL]
    rgb_of = np.array([[(c & 15) * 17, ((c >> 4) & 15) * 17, ((c >> 8) & 15) * 17] for c in full])
    # palette strip
    pal = np.zeros((16 * 8, 16 * 8, 3), dtype=np.uint8)
    for i in range(256):
        r, s = divmod(i, 16)
        pal[r * 8:(r + 1) * 8, s * 8:(s + 1) * 8] = rgb_of[i]
    write_png(os.path.join(dirname, "palette.png"), pal)
    # textures sheet, 3x scale, 4 per row
    scale = 3
    per_row = 4
    n = len(TEXTURES) + len(FLATS)
    rows = (n + per_row - 1) // per_row
    sheet = np.zeros((rows * (TEX * scale + 4), per_row * (TEX * scale + 4), 3), dtype=np.uint8)
    for k, (_, img) in enumerate(TEXTURES + FLATS):
        r, c = divmod(k, per_row)
        rgb = rgb_of[img]
        rgb = np.repeat(np.repeat(rgb, scale, 0), scale, 1)
        y0 = r * (TEX * scale + 4)
        x0 = c * (TEX * scale + 4)
        sheet[y0:y0 + TEX * scale, x0:x0 + TEX * scale] = rgb
    write_png(os.path.join(dirname, "textures.png"), sheet)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    out_path = sys.argv[1]
    preview_dir = None
    if "--preview" in sys.argv:
        preview_dir = sys.argv[sys.argv.index("--preview") + 1]

    shade_tables = build_shade_tables()
    build_textures()

    out = [
        "# Generated by tools/gen_assets.py -- do not edit by hand.",
        "  .rodata",
        "  .align 4",
    ]
    emit_halfs(out, "shade_tables", shade_tables.reshape(-1))
    out.append("  .align 4")
    # 16.16 sine for 4096 angle steps per turn (0 = +x, 1024 = +y).
    sines = [int(round(np.sin(2 * np.pi * a / 4096) * 65536)) for a in range(4096)]
    out.append("  .global sin_table")
    out.append("sin_table:")
    emit_words(out, sines)
    # Quarter squares Q(n) = floor(n * n / 4) for n = -510..510, used by
    # umul16 in gfx.s; qsq_center labels Q(0) (an even index, so it stays
    # word aligned and the halves before it pack evenly).
    q = [(n * n) // 4 for n in range(-510, 511)]
    emit_halfs(out, "qsq_table", q[:510])
    emit_halfs(out, "qsq_center", q[510:])
    out.append("  .align 4")
    # Walls are sampled down columns, so store each texture column-major.
    out.append("  .global wall_textures")
    out.append("wall_textures:")
    for name, img in TEXTURES:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.T.reshape(-1))
    out.append("  .align 4")
    # Floors and ceilings are sampled at arbitrary (u, v), stored row-major.
    out.append("  .global flat_textures")
    out.append("flat_textures:")
    for name, img in FLATS:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.reshape(-1))
    out.append("  .align 4")
    emit_bytes(out, "font_small", small_font_bytes())
    here = os.path.dirname(os.path.abspath(__file__))
    rows, spans = big_font_bytes(os.path.join(here, "..", "..", "..", "common", "text_tiles.s"))
    emit_bytes(out, "font_big", rows)
    emit_bytes(out, "font_big_spans", spans)
    out.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(out))
    if preview_dir:
        preview(preview_dir, shade_tables)
    print(f"wrote {out_path}: {len(TEXTURES)} textures")


if __name__ == "__main__":
    main()
