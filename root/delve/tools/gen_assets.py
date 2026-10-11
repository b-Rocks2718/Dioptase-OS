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


SPR = 32  # billboard sprite size


class Canvas:
    """A 32x32 sprite canvas of palette indices (0 = transparent)."""

    def __init__(self):
        self.px = np.zeros((SPR, SPR), dtype=np.int64)

    def set(self, x, y, ramp, shade):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < SPR and 0 <= y < SPR:
            self.px[y, x] = idx(ramp, max(1, min(15, shade)))

    def outline(self, ramp=GRAY, shade=1):
        """Add a dark one-pixel outline around opaque pixels."""
        src = self.px.copy()
        for y in range(SPR):
            for x in range(SPR):
                if src[y, x]:
                    continue
                for (dy, dx) in ((0, 1), (1, 0), (0, -1), (-1, 0)):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < SPR and 0 <= xx < SPR and src[yy, xx]:
                        self.px[y, x] = idx(ramp, shade)
                        break


def shaded_blob(c, cx, cy, rx, ry, ramp, base=8, light=(-0.55, -0.65), spec=True,
                squash_bottom=True):
    """Fill an ellipse with sphere-like shading lit from `light`."""
    lx, ly = light
    lz = np.sqrt(max(0.0, 1 - lx * lx - ly * ly))
    for y in range(SPR):
        for x in range(SPR):
            nx = (x + 0.5 - cx) / rx
            ny = (y + 0.5 - cy) / ry
            if squash_bottom and ny > 0:
                ny *= 1.0
            d = nx * nx + ny * ny
            if d > 1.0:
                continue
            nz = np.sqrt(1 - d)
            diff = max(0.0, nx * lx + ny * ly + nz * lz)
            shade = base - 4 + diff * 8
            if spec and diff > 0.93:
                shade = 15
            c.set(x, y, ramp, shade)


def sprite_slime(phase):
    """Green slime: squashes and stretches as it hops (phase 0..3)."""
    c = Canvas()
    squash = [0.0, 0.12, 0.0, -0.12][phase % 4]
    rx = 12 * (1 + squash)
    ry = 9 * (1 - squash)
    cy = 31 - ry
    shaded_blob(c, 16, cy, rx, ry, SLIME, base=8)
    # flat bottom shadow line
    for x in range(int(16 - rx) + 1, int(16 + rx)):
        c.set(x, 30, SLIME, 3)
    # eyes
    ey = int(cy - ry * 0.25)
    for ex in (12, 20):
        c.set(ex, ey, GRAY, 15)
        c.set(ex, ey + 1, GRAY, 15)
        c.set(ex + (1 if ex < 16 else 0), ey + 1, GRAY, 1)
    # mouth
    for x in range(14, 19):
        c.set(x, ey + 4, SLIME, 2)
    # floating bits inside
    c.set(10, int(cy + 2), SLIME, 12)
    c.set(22, int(cy + 3), SLIME, 11)
    c.outline(SLIME, 1)
    return c.px


def sprite_bat(phase):
    """Cave bat with flapping wings (phase 0..3)."""
    c = Canvas()
    flap = [-5, 0, 5, 0][phase % 4]
    shaded_blob(c, 16, 16, 4.5, 5.5, PURPLE, base=6)
    for side in (-1, 1):
        for i in range(12):
            x = 16 + side * (4 + i)
            tip = flap * (i / 11.0)
            top = 13 + tip - (3 if i < 6 else 1) * np.sin(i / 11.0 * np.pi)
            bottom = 18 + tip * 0.8 - (i % 4 == 3) * 2
            for y in range(int(top), int(bottom)):
                c.set(x, y, PURPLE, 5 + (y == int(top)) * 3)
    for ex in (14, 18):
        c.set(ex, 14, RED, 14)
    c.set(15, 18, GRAY, 15)
    c.set(17, 18, GRAY, 15)
    # ears
    c.set(13, 10, PURPLE, 6)
    c.set(19, 10, PURPLE, 6)
    c.outline(PURPLE, 1)
    return c.px


def sprite_eye(phase):
    """Floating eye; phase 0..2 looks around, 3 is mid-blink, 4 charges."""
    c = Canvas()
    shaded_blob(c, 16, 15, 11, 11, BONE, base=10)
    # veins
    rng = np.random.default_rng(99)
    for _ in range(6):
        a = rng.uniform(0, 2 * np.pi)
        for r in np.linspace(7, 10.5, 6):
            c.set(16 + np.cos(a) * r, 15 + np.sin(a) * r, RED, 8)
            a += rng.uniform(-0.25, 0.25)
    look = [(0, 0), (-3, 1), (3, 1), (0, 0), (0, 0)][phase % 5]
    ix, iy = 16 + look[0], 15 + look[1]
    iris_ramp = FIRE if phase == 4 else RED
    for y in range(SPR):
        for x in range(SPR):
            d = np.hypot(x + 0.5 - ix, y + 0.5 - iy)
            if d < 5.5:
                c.set(x, y, iris_ramp, 6 + (5.5 - d) * 1.4)
            if d < 2.5:
                c.set(x, y, GRAY, 1)
    c.set(ix - 2, iy - 3, GRAY, 15)
    if phase == 3:
        for y in range(5, 25):
            for x in range(6, 27):
                if c.px[y, x] and abs(y - 15) > 2:
                    c.set(x, y, BONE, 6 if y < 15 else 5)
    # tentacles below
    for t, x0 in enumerate((10, 16, 22)):
        for i in range(6):
            x = x0 + np.sin(i * 0.9 + phase + t) * 1.2
            c.set(x, 26 + i, PINK, 7 - i // 2)
    c.outline(RED, 2)
    return c.px


def sprite_wraith(phase):
    """Translucent-looking wraith with a waving hem (phase 0..3)."""
    c = Canvas()
    for y in range(4, 31):
        half = 4 + min(y - 4, 10) * 0.8
        for x in range(SPR):
            dx = x + 0.5 - 16
            wave = np.sin(x * 0.7 + phase * 1.5) * 2
            if abs(dx) < half and y < 28 + wave:
                shade = 11 - abs(dx) / half * 5 + (y < 10) * 2
                if (x + y) % 2 == 0 and y > 20:
                    shade -= 2
                c.set(x, y, ICE, shade)
    for ex in (13, 19):
        for dy in range(3):
            c.set(ex, 10 + dy, BLUE, 2)
            c.set(ex + 1, 10 + dy, BLUE, 2)
    c.set(13, 10, ICE, 15)
    c.set(19, 10, ICE, 15)
    for x in range(14, 19):
        c.set(x, 16, BLUE, 3)
    # arms reaching out
    for i in range(6):
        y = 15 + i * 0.6 + np.sin(phase + i) * 0.5
        c.set(9 - i, y, ICE, 9)
        c.set(23 + i, y, ICE, 9)
    c.outline(BLUE, 3)
    return c.px


def sprite_crystal(phase):
    """Dioptase crystal cluster on the floor with a moving glint."""
    c = Canvas()
    shards = [(16, 30, 4.5, 15, 0.0), (11, 30, 3.0, 9, -0.35), (21, 30, 3.0, 10, 0.3)]
    for (bx, by, w, h, lean) in shards:
        for y in range(int(by - h), by):
            t = (by - y) / h
            half = w * (1.0 if t < 0.75 else (1 - t) / 0.25)
            cx = bx + lean * (by - y)
            for x in range(SPR):
                dx = x + 0.5 - cx
                if abs(dx) <= half:
                    facet = 12 if dx < -half * 0.3 else (7 if dx > half * 0.3 else 10)
                    c.set(x, y, GEM, facet)
    glint_y = 30 - (phase % 4) * 4 - 3
    for d in range(-1, 2):
        c.set(16 + d, glint_y, GRAY, 15)
        c.set(16, glint_y + d, GRAY, 15)
    c.outline(GEM, 2)
    return c.px


def sprite_potion(ramp):
    """Round-bottomed flask with liquid and a cork."""
    c = Canvas()
    shaded_blob(c, 16, 24, 7, 7, ramp, base=9)
    for y in range(12, 19):
        for x in range(13, 19):
            c.set(x, y, ICE, 11 if x < 15 else 8)
    for y in range(9, 12):
        for x in range(13, 19):
            c.set(x, y, WOOD, 8 if x < 16 else 6)
    c.set(13, 21, GRAY, 15)
    c.set(13, 22, GRAY, 13)
    c.outline(GRAY, 1)
    return c.px


def sprite_key():
    c = Canvas()
    for y in range(SPR):
        for x in range(SPR):
            d = np.hypot(x + 0.5 - 10, y + 0.5 - 22)
            if 2.2 < d < 5.2:
                c.set(x, y, GOLD, 12 - (y > 22) * 4)
    for x in range(14, 27):
        c.set(x, 21, GOLD, 13)
        c.set(x, 22, GOLD, 9)
    for x in (21, 24):
        for y in range(23, 26):
            c.set(x, y, GOLD, 8)
    c.outline(GOLD, 2)
    return c.px


def sprite_portal(phase):
    """Swirling green descent portal (phase 0..3 rotates the arms)."""
    c = Canvas()
    for y in range(SPR):
        for x in range(SPR):
            dx = x + 0.5 - 16
            dy = (y + 0.5 - 16) * 1.0
            r = np.hypot(dx, dy)
            if r > 15:
                continue
            a = np.arctan2(dy, dx)
            swirl = np.sin(a * 3 + r * 0.55 - phase * np.pi / 2)
            shade = 4 + (15 - r) * 0.45 + swirl * 3
            if r < 3:
                shade = 15
            c.set(x, y, GEM if swirl > -0.2 else ICE, shade)
    return c.px


def sprite_orb(ramp, radius=6.0):
    """Glowing spell orb: bright core fading to a soft rim."""
    c = Canvas()
    for y in range(SPR):
        for x in range(SPR):
            r = np.hypot(x + 0.5 - 16, y + 0.5 - 16)
            if r < radius:
                c.set(x, y, ramp, 15 - (r / radius) * 7)
            elif r < radius + 2.5 and (x + y) % 2 == 0:
                c.set(x, y, ramp, 6)
    return c.px


def sprite_puff(phase, ramp):
    """Expanding burst for impacts and deaths (phase 0..3)."""
    c = Canvas()
    rng = np.random.default_rng(7 + phase)
    radius = 4 + phase * 3.5
    for _ in range(90):
        a = rng.uniform(0, 2 * np.pi)
        r = rng.uniform(0, radius)
        shade = 15 - (r / radius) * 6 - phase * 2
        c.set(16 + np.cos(a) * r, 16 + np.sin(a) * r, ramp, shade)
    return c.px


def overlay_staff():
    """The player's crystal-tipped staff: 32x64 palette indices, held from the
    lower right and leaning into the view."""
    img = np.zeros((64, 32), dtype=np.int64)

    def put(x, y, ramp, shade):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < 32 and 0 <= y < 64:
            img[y, x] = idx(ramp, max(1, min(15, shade)))

    # shaft from the bottom right up toward the upper left
    for t in np.linspace(0, 1, 160):
        cx = 27 - t * 13 + np.sin(t * 9) * 0.8
        cy = 63 - t * 50
        for w in range(-2, 3):
            shade = 7 - w * 1.5 + np.sin(t * 40) * 0.6
            put(cx + w, cy, WOOD, shade)
    # crystal head
    for y in range(0, 16):
        for x in range(6, 22):
            dx = x + 0.5 - 14
            dy = y + 0.5 - 8
            if abs(dx) * 1.6 + abs(dy) <= 8.5:
                facet = 13 if dx < -1 else (8 if dx > 1.5 else 11)
                if abs(dx) * 1.6 + abs(dy) > 7.5:
                    facet = 4
                put(x, y, GEM, facet - (y > 10) * 2)
    put(12, 4, GRAY, 15)
    put(12, 5, GRAY, 15)
    put(11, 5, GEM, 15)
    # claw prongs holding the crystal
    for (x, y) in ((9, 14), (10, 15), (18, 14), (17, 15), (13, 16), (14, 16), (15, 16)):
        put(x, y, GOLD, 9)
    # hand gripping the shaft
    for y in range(36, 50):
        for x in range(15, 30):
            dx = x + 0.5 - 22
            dy = y + 0.5 - 43
            if (dx / 7.0) ** 2 + (dy / 6.5) ** 2 <= 1.0:
                shade = 10 - dy * 0.35 - dx * 0.15
                if abs(dy) < 6 and (int(y) - 37) % 4 == 3 and dx < 3:
                    shade = 5   # finger creases
                put(x, y, BONE, shade)
    for x in range(14, 30):
        for y in range(50, 64):
            if x - 14 > (y - 50) * 0.3:
                put(x, y, BLUE, 4 + (x - 14) * 0.15)   # sleeve
    return img


def overlay_cursor():
    """Crosshair: 11x11 cross with a hollow center, centered in 32x32."""
    img = np.zeros((32, 32), dtype=np.int64)
    for d in range(2, 6):
        for (x, y) in ((16 + d, 16), (16 - d, 16), (16, 16 + d), (16, 16 - d)):
            img[y, x] = idx(GRAY, 15)
    for (x, y) in ((17, 17), (15, 15), (17, 15), (15, 17)):
        img[y, x] = idx(GEM, 12)
    out = img.copy()
    for y in range(32):
        for x in range(32):
            if img[y, x]:
                continue
            for (dy, dx) in ((0, 1), (1, 0), (0, -1), (-1, 0)):
                yy, xx = y + dy, x + dx
                if 0 <= yy < 32 and 0 <= xx < 32 and img[yy, xx] and img[yy, xx] != idx(GEM, 12):
                    out[y, x] = idx(GRAY, 1)
                    break
    return out


def to_sprite_pixels(img, full_table):
    """Palette indices -> 12-bit hardware sprite pixels (0xF000 = clear)."""
    flat = img.reshape(-1)
    return [0xF000 if v == 0 else int(full_table[v]) for v in flat]


SPRITES = []  # (name, 32x32 row-major palette indices)


def build_sprites():
    for p in range(4):
        SPRITES.append((f"slime{p}", sprite_slime(p)))
    for p in range(4):
        SPRITES.append((f"bat{p}", sprite_bat(p)))
    for p in range(5):
        SPRITES.append((f"eye{p}", sprite_eye(p)))
    for p in range(4):
        SPRITES.append((f"wraith{p}", sprite_wraith(p)))
    for p in range(4):
        SPRITES.append((f"crystal{p}", sprite_crystal(p)))
    SPRITES.append(("potion_hp", sprite_potion(RED)))
    SPRITES.append(("potion_mp", sprite_potion(BLUE)))
    SPRITES.append(("key", sprite_key()))
    for p in range(4):
        SPRITES.append((f"portal{p}", sprite_portal(p)))
    SPRITES.append(("orb_spark", sprite_orb(ICE, 4.5)))
    SPRITES.append(("orb_fire", sprite_orb(FIRE, 6.5)))
    SPRITES.append(("orb_enemy", sprite_orb(PINK, 5.0)))
    for p in range(4):
        SPRITES.append((f"puff_fire{p}", sprite_puff(p, FIRE)))
    for p in range(4):
        SPRITES.append((f"puff_gem{p}", sprite_puff(p, GEM)))


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
# Music: a tiny tracker. Each song has four channels (lead, arpeggio, bass,
# drums); each row is one eighth note. Tokens: a note like "D4" or "Bb3",
# "-" to hold, "." for silence. Drum tokens: "K" kick, "S" snare, "h" hat.
# ---------------------------------------------------------------------------
AUDIO_RATE = 25000
NOTE_NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
# Drum "notes" set the noise channel's LFSR clock: low = thump, high = hiss.
DRUM_NOTES = {"K": 57, "S": 100, "h": 120}
ROW_HOLD = 0
ROW_OFF = 1


def note_number(tok):
    if tok in DRUM_NOTES:
        return DRUM_NOTES[tok]
    name = tok[0]
    rest = tok[1:]
    semis = NOTE_NAMES[name]
    while rest and rest[0] in "#b":
        semis += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    octave = int(rest)
    return 12 * (octave + 1) + semis


def parse_track(text):
    rows = []
    for tok in text.replace("|", " ").split():
        if tok == "-":
            rows.append(ROW_HOLD)
        elif tok == ".":
            rows.append(ROW_OFF)
        else:
            rows.append(note_number(tok))
    return rows


def arp(chords, pattern=(0, 1, 2, 1, 0, 1, 2, 1)):
    """Broken-chord eighths: one bar per chord, chord given as note names."""
    out = []
    for chord in chords:
        notes = chord.split()
        for i in pattern:
            out.append(notes[i % len(notes)])
    return " ".join(out)


def bass_line(roots, style):
    out = []
    for r in roots:
        lo = r
        hi = r[:-1] + str(int(r[-1]) + 1)
        if style == "drive":
            out += [lo, lo, hi, lo, lo, lo, hi, lo]
        else:
            out += [lo, "-", "-", "-", hi, "-", lo, "-"]
    return " ".join(out)


SONGS = []


def add_song(name, bpm, lead, arpeggio, bass, drums, loop=True):
    tracks = [parse_track(t) for t in (lead, arpeggio, bass, drums)]
    length = max(len(t) for t in tracks)
    for t in tracks:
        while len(t) < length:
            t.append(ROW_HOLD if t else ROW_OFF)
    row_samples = int(AUDIO_RATE * 60 / bpm / 2)
    SONGS.append((name, row_samples, length, loop, tracks))


def build_music():
    # 1: title -- slow A minor, bell lead over a soft arpeggio
    add_song(
        "title", 84,
        lead=". . . . . . . . | " * 4 +
             "E5 - - - D5 - C5 - | C5 - - - A4 - - - | B4 - - - G4 - D5 - | E5 - - - - - - - | "
             "F5 - E5 - D5 - C5 - | E5 - - - A4 - C5 - | B4 - G#4 - E4 - B4 - | A4 - - - - - - . |",
        arpeggio=arp(["A3 C4 E4", "F3 A3 C4", "G3 B3 D4", "E3 G3 B3"] * 2 +
                     ["A3 C4 E4", "F3 A3 C4", "G3 B3 D4", "E3 G3 B3",
                      "D3 F3 A3", "A3 C4 E4", "E3 G#3 B3", "A3 C4 E4"]),
        bass=bass_line(["A2", "F2", "G2", "E2"] * 3 + ["D2", "A2", "E2", "A2"], "slow"),
        drums="",
    )
    # 2: dungeon -- driving D minor
    kit = "K h S h K h S h | " * 16
    add_song(
        "dungeon", 112,
        lead=". . . . . . . . | " * 8 +
             "D5 - - F5 - E5 D5 - | A4 - - - - - - . | Bb4 - - D5 - C5 Bb4 - | C5 - - - - - E5 - | "
             "F5 - E5 - D5 - C5 - | D5 - - - A4 - - . | G4 - Bb4 - D5 - G5 - | E5 - - - C#5 - - . |",
        arpeggio=arp(["D4 F4 A4", "D4 F4 A4", "Bb3 D4 F4", "C4 E4 G4",
                      "D4 F4 A4", "D4 F4 A4", "G3 Bb3 D4", "A3 C#4 E4"] * 2),
        bass=bass_line(["D2", "D2", "Bb1", "C2", "D2", "D2", "G1", "A1"] * 2, "drive"),
        drums=kit,
    )
    # 3: victory fanfare (plays once)
    add_song(
        "victory", 120,
        lead="D5 F#5 A5 D6 - - A5 - | B5 - - G5 - - A5 - | F#5 - D5 - E5 - - - | D5 - - - - - - . |",
        arpeggio="D4 . D4 . D4 . . . | G4 . G4 . E4 . . . | D4 . A3 . C#4 . . . | D4 - - - - - - . |",
        bass="D3 - - - - - - - | G2 - - - E2 - - - | D2 - A2 - A1 - - - | D2 - - - - - - . |",
        drums="K . . . S . . . | K . . . S . . . | K . K . S . S . | K - - - - - - . |",
        loop=False,
    )


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
    # sprites sheet, 4x scale, 8 per row, on a dark checker background
    scale = 4
    per_row = 8
    rows = (len(SPRITES) + per_row - 1) // per_row
    cell = SPR * scale + 4
    sheet = np.zeros((rows * cell, per_row * cell, 3), dtype=np.uint8)
    for k, (_, img) in enumerate(SPRITES):
        r, c = divmod(k, per_row)
        rgb = rgb_of[img].astype(np.uint8)
        check = ((np.arange(SPR)[:, None] // 4 + np.arange(SPR)[None, :] // 4) % 2) * 20 + 30
        rgb = np.where((img == 0)[:, :, None], check[:, :, None].astype(np.uint8), rgb)
        rgb = np.repeat(np.repeat(rgb, scale, 0), scale, 1)
        sheet[r * cell:r * cell + SPR * scale, c * cell:c * cell + SPR * scale] = rgb
    write_png(os.path.join(dirname, "sprites.png"), sheet)


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
    build_sprites()
    build_music()

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
    # Billboards: column-major texels plus each column's opaque [top, bottom)
    # span so the renderer can skip transparent rows and empty columns.
    out.append("  .global sprite_texels")
    out.append("sprite_texels:")
    spans = []
    for name, img in SPRITES:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.T.reshape(-1))
        for x in range(SPR):
            col = np.nonzero(img[:, x])[0]
            spans.extend([int(col[0]), int(col[-1]) + 1] if len(col) else [0, 0])
    emit_bytes(out, "sprite_spans", spans)
    out.append("  .align 4")
    full = shade_tables[LIGHT_FULL]
    staff = overlay_staff()
    emit_halfs(out, "overlay_staff", to_sprite_pixels(staff[:32], full) + to_sprite_pixels(staff[32:], full))
    emit_halfs(out, "overlay_cursor", to_sprite_pixels(overlay_cursor(), full))
    out.append("  .align 4")
    # Phase increment per sample for each MIDI note at 25 kHz (2^32 / rate).
    incs = [int(round(440.0 * 2 ** ((n - 69) / 12.0) * 2 ** 32 / AUDIO_RATE)) & 0xFFFFFFFF for n in range(128)]
    out.append("  .global note_incs")
    out.append("note_incs:")
    emit_words(out, incs)
    # Songs: a directory of (row_samples, rows, loop, data offset) words,
    # then row-major data with 4 bytes (lead, arp, bass, drums) per row.
    out.append("  .global song_table")
    out.append("song_table:")
    offset = 0
    dir_words = []
    for name, row_samples, length, loop, tracks in SONGS:
        dir_words += [row_samples, length, 1 if loop else 0, offset]
        offset += length * 4
    emit_words(out, dir_words)
    data = []
    for name, row_samples, length, loop, tracks in SONGS:
        for r in range(length):
            data += [tracks[c][r] for c in range(4)]
    emit_bytes(out, "song_data", data)
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
    print(f"wrote {out_path}: {len(TEXTURES)} textures, {len(FLATS)} flats, {len(SPRITES)} sprites, {len(SONGS)} songs")
    for name, row_samples, length, loop, tracks in SONGS:
        print(f"  song {name}: {length} rows, {length * row_samples / AUDIO_RATE:.1f} s")
    for i, (name, _) in enumerate(SPRITES):
        print(f"  sprite {i:2d} {name}")


if __name__ == "__main__":
    main()
