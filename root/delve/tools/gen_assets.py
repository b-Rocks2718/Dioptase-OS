#!/usr/bin/env python3
"""Generate DELVE's art assets as Dioptase assembler data (assets.s).

Everything is procedural: a 256-color palette made of 16 ramps x 16 shades,
the per-light-level color tables the renderer uses for shading, wall/floor
textures, billboard sprites, the player's staff, the crosshair sprite, the
music, and the 8x8 font (decoded from the OS's own text tiles).

Texture and sprite pixels are palette indices (ramp * 16 + shade). Index 0 is
black in walls and "transparent" in billboards. The renderer converts indices
to 12-bit 0x0BGR VGA colors through `shade_tables` (docs/mem_map.md).

Usage: gen_assets.py <out.s> [--preview <dir>]
"""
import sys
import os

import numpy as np

# root/ is packaged into the guest filesystem image, so keep it free of
# __pycache__ directories.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngout import write_png  # noqa: E402
import music  # noqa: E402
import songs  # noqa: E402

RNG = np.random.default_rng(0xD10D)
TEX = 64  # floor/ceiling texture size in texels, and the walls' design grid
WALL_SCALE = 2            # wall textures are rasterized this much finer than
WALL = TEX * WALL_SCALE   # their design grid: WALL texels square at mip level 0
WALL_MIPS = 3             # mip levels of WALL, WALL / 2, and WALL / 4 texels

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


WARM = np.array([64.0, 34.0, 6.0])   # overbright light leans toward torch orange


def light_factor(level):
    if level <= LIGHT_FULL:
        return (level / LIGHT_FULL) ** 1.15
    return 1.0 + (level - LIGHT_FULL) * 0.11


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
                c = PALETTE[i] * (1.0 + (f - 1.0) * 0.6) + (f - 1.0) * WARM * 2.2
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
# Wall textures. Each returns a (WALL, WALL) array of palette indices in
# row-major [y][x] order; main() stores them column-major for the renderer,
# with smaller mip levels made by halve(). They are laid out on the TEX-texel
# design grid and drawn WALL_SCALE times finer: the coarse structure (bricks,
# planks, frames) follows the design grid, while noise, grain, cracks, glyphs,
# crystals, and round shapes are evaluated per fine texel. Fine texel i's
# center lies at design coordinate DESIGN[i] = (i + 0.5) / WALL_SCALE - 0.5,
# which puts design texel k's center at k.
# ---------------------------------------------------------------------------
DESIGN = (np.arange(WALL) + 0.5) / WALL_SCALE - 0.5


def child_rng(rng):
    """A generator for detail that exists only at WALL resolution. Spawning
    it leaves `rng`'s own stream alone, so the layout drawn from `rng`
    afterwards (brick tones, cracks, crystals, glyphs) is the one the textures
    were designed with at 64 texels."""
    return rng.spawn(1)[0]


def fine_grain(rng):
    """Per-texel white noise in [0, 1) at WALL resolution. The 64-texel
    designs drew TEX x TEX grain here; that draw is still made and dropped to
    keep the rest of `rng`'s stream, and therefore the layout, unchanged."""
    rng.random((TEX, TEX))
    return child_rng(rng).random((WALL, WALL))


def segment_distance(xs, ys, a, b):
    """Distance from each point (xs, ys) to the segment from a to b."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    length2 = dx * dx + dy * dy
    t = 0.0
    if length2 > 0:
        t = np.clip(((xs - a[0]) * dx + (ys - a[1]) * dy) / length2, 0.0, 1.0)
    return np.hypot(xs - (a[0] + t * dx), ys - (a[1] + t * dy))


def draw_crack(shade, walk, dark=1.5):
    """Draw a one-texel crack at WALL resolution through the centers of the
    design texels `walk` [(x, y), ...]. The texel right of the groove is its
    far wall, which faces the upper-left light, so it is brightened."""
    s = WALL_SCALE
    pts = [(int(x) * s + s // 2, int(y) * s + s // 2) for x, y in walk]
    crack = set()
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] or pts):
        steps = max(abs(x1 - x0), abs(y1 - y0), 1)
        for t in range(steps + 1):
            crack.add(((y0 + (y1 - y0) * t // steps) % WALL,
                       (x0 + (x1 - x0) * t // steps) % WALL))
    for y, x in crack:
        shade[y, x] = dark
    for y, x in crack:
        if (y, (x + 1) % WALL) not in crack:
            shade[y, (x + 1) % WALL] += 1.0


def rivet(ramps, shade, cx, cy, radius=1.6, ramp=GRAY):
    """Draw a round rivet head centered on WALL-texel coordinates (cx, cy),
    lit from the upper left, with its shadow cast down and to the right."""
    for y in range(int(cy - radius) - 1, int(cy + radius) + 3):
        for x in range(int(cx - radius) - 1, int(cx + radius) + 3):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= radius * radius:
                ramps[y, x] = ramp
                shade[y, x] = 9.5 - (dx + dy) / radius * 2.5
            elif (dx - 1) ** 2 + (dy - 1) ** 2 <= radius * radius:
                shade[y, x] = 2.5


def bricks(ramp=STONE, rows=8, cols=4, mortar_shade=2.0, base=7.0, moss=0.0, rng=RNG):
    s = WALL_SCALE
    h = TEX // rows   # brick size on the design grid
    w = TEX // cols
    noise = fbm(4, 8, rng, WALL)
    grain = fine_grain(rng)
    shade = np.zeros((WALL, WALL))
    ramps = np.full((WALL, WALL), ramp)
    for by in range(rows):
        offset = (w // 2) * (by % 2)
        for bx in range(cols + 1):
            tone = base + rng.normal(0, 1.1)
            x0 = (bx * w - offset) * s   # brick corner in fine texels
            y0 = by * h * s
            for y in range(y0, y0 + h * s):
                ly = y - y0
                for xx in range(x0, x0 + w * s):
                    x = xx % WALL
                    lx = xx - x0
                    if ly >= (h - 1) * s or lx >= (w - 1) * s:
                        shade[y, x] = mortar_shade + grain[y, x]
                        continue
                    v = tone + (noise[y, x] - 0.5) * 4.0 + (grain[y, x] - 0.5) * 1.2
                    # Bevels, one design texel wide, fading inward: the top
                    # and left edges catch the light, the others are shadowed.
                    lit = min(ly, lx)
                    dim = min((h - 1) * s - 1 - ly, (w - 1) * s - 1 - lx)
                    if lit < s:
                        v += 1.6 - 0.6 * lit / s
                    elif dim < s:
                        v -= 1.4 - 0.5 * dim / s
                    shade[y, x] = v
    if moss > 0:
        m = fbm(4, 4, rng, WALL)
        ys = np.arange(WALL)[:, None] / WALL
        # the grain term frays the moss edge at the fine scale
        mask = (m + ys * 0.55 + (grain - 0.5) * 0.08) > (1.25 - moss)
        ramps = np.where(mask, MOSS, ramps)
        shade = np.where(mask, shade * 0.85 + 1.0, shade)
    # a few hairline cracks, each a random walk down the design grid
    for _ in range(3):
        x, y = rng.integers(0, TEX, 2)
        walk = []
        for _ in range(rng.integers(6, 14)):
            walk.append((x, y))
            x += rng.integers(-1, 2)
            y += 1
        draw_crack(shade, walk)
    return idx(ramps, np.clip(shade, 1, 14))


def cave_rock(ramp=STONE, rng=RNG, crystals=0, size=TEX):
    """Mottled rock with dark ridges, `size` texels square: TEX for flats,
    WALL for walls, which also get grit finer than the design grid. Crystals
    need the WALL size."""
    n = fbm(5, 4, rng, size)
    ridges = 1.0 - np.abs(fbm(4, 8, rng, size) * 2 - 1)
    shade = 2.5 + n * 9.0 - (ridges > 0.86) * 3.0
    if size > TEX:
        shade += (child_rng(rng).random((size, size)) - 0.5) * 1.2
    ramps = np.full((size, size), ramp)
    out = idx(ramps, np.clip(shade, 1, 13))
    for _ in range(crystals):
        cx, cy = rng.integers(6, TEX - 6, 2)
        crystal_size = rng.integers(3, 7)
        draw_crystal(out, cx, cy, crystal_size, rng)
    return out


def draw_crystal(img, cx, cy, size, rng):
    """Draw a faceted hexagonal dioptase crystal (lit from the upper left)
    centered on design texel (cx, cy) of a WALL-texel image."""
    angle = rng.uniform(-0.6, 0.6)
    ca, sa = np.cos(angle), np.sin(angle)
    s = WALL_SCALE
    for fy in range((cy - size * 2) * s, (cy + size * 2 + 1) * s):
        for fx in range((cx - size * 2) * s, (cx + size * 2 + 1) * s):
            dx, dy = DESIGN[fx % WALL] - cx, DESIGN[fy % WALL] - cy
            u = dx * ca + dy * sa
            v = -dx * sa + dy * ca
            # elongated hexagon: |v| < size, |u| + |v|/2 < size*1.6
            if abs(v) <= size and abs(u) + abs(v) * 0.6 <= size * 1.7:
                facet = 0
                if u < -size * 0.6:
                    facet = 1
                elif u > size * 0.6:
                    facet = 2
                shade = 9 + (-v / size) * 3 + (3 if facet == 1 else (-2 if facet == 2 else 0))
                if facet == 0 and u < 0 and abs(v + size * 0.45) < 0.35:
                    shade = 14   # glint along the upper part of the front facet
                if abs(v) >= size - 0.5 or abs(u) + abs(v) * 0.6 >= size * 1.7 - 0.8:
                    shade = 4
                img[fy % WALL, fx % WALL] = idx(GEM, min(15, max(2, shade)))


def wood_door(rng=RNG):
    s = WALL_SCALE
    shade = np.zeros((WALL, WALL))
    ramps = np.full((WALL, WALL), WOOD)
    grain = fbm(3, 4, rng, WALL)
    detail = child_rng(rng)
    fibers = detail.random(WALL)          # streaks along each texel column
    speck = detail.random((WALL, WALL))
    planks = 5
    pw = TEX / planks
    for x in range(WALL):
        p = int(DESIGN[x] / pw)
        tone = 6.5 + ((p * 7) % 3 - 1) * 0.7 + (fibers[x] - 0.5) * 0.9
        lx = DESIGN[x] - p * pw
        for y in range(WALL):
            g = np.sin((DESIGN[y] * 0.35 + grain[y, x] * 9 + p * 3.1)) * 0.9
            v = tone + g + (grain[y, x] - 0.5) * 2 + (speck[y, x] - 0.5) * 0.5
            if lx < 0.75:
                v = 2.5        # gap between planks
            elif lx < 1.75:
                v += 1.2       # plank edge catching the light
            shade[y, x] = v
    # stone frame, three design texels wide, darker toward the outside
    yi = np.arange(WALL)[:, None] // s
    xi = np.arange(WALL)[None, :] // s
    edge = np.minimum(np.minimum(yi, xi), np.minimum(TEX - 1 - yi, TEX - 1 - xi))
    frame = edge < 3
    ramps[frame] = STONE
    shade[frame] = (4 + edge + (speck - 0.5) * 0.8)[frame]
    # iron bands with rivets
    for by in (12, 48):
        for y in range(by * s, (by + 5) * s):
            ly = y - by * s
            for x in range(3 * s, (TEX - 3) * s):
                ramps[y, x] = GRAY
                v = 5 + (speck[y, x] - 0.5) * 0.6
                if ly < 2:
                    v += 3 - ly                        # lit top edge
                elif ly >= 5 * s - 2:
                    v -= 1 + (ly - (5 * s - 2))        # shadowed bottom edge
                shade[y, x] = v
        for x in range(8, TEX - 5, 12):
            rivet(ramps, shade, x * s + 0.5, (by + 2) * s + 0.5)
    # ring handle, with its shadow on the planks
    cx, cy = 46 * s + 0.5, 34 * s + 0.5
    radius = 4 * s
    for y in range(int(cy - radius) - 3, int(cy + radius) + 4):
        for x in range(int(cx - radius) - 3, int(cx + radius) + 4):
            if abs(np.hypot(x - cx, y - cy) - radius) <= 1.2:
                ramps[y, x] = GOLD
                # brighter on the upper half and toward the upper left
                shade[y, x] = 10 - (y - cy) / radius * 2.0 - (x - cx) / radius * 0.8
            elif abs(np.hypot(x - cx - 1.5, y - cy - 1.5) - radius) <= 1.0:
                shade[y, x] = min(shade[y, x], 2.5)
    return idx(ramps, np.clip(shade, 1, 15))


def iron_door(rng=RNG):
    s = WALL_SCALE
    n = fbm(4, 8, rng, WALL)
    brushed = child_rng(rng).random((WALL, WALL))
    shade = 4.5 + n * 3 + (brushed - 0.5) * 0.5
    ramps = np.full((WALL, WALL), DEEP)
    # steel frame, four design texels wide, with a lit outer lip and a
    # shadow line where it meets the panel
    fy = np.arange(WALL)[:, None]
    fx = np.arange(WALL)[None, :]
    edge = np.minimum(np.minimum(fy, fx), np.minimum(WALL - 1 - fy, WALL - 1 - fx))
    frame = edge < 4 * s
    ramps[frame] = GRAY
    frame_shade = (6 + (brushed - 0.5) * 0.6 + 3 * (edge == 0) + 1.5 * (edge == 1)
                   - 2 * (edge == 4 * s - 1))
    shade[frame] = frame_shade[frame]
    for y in range(8, TEX - 8, 8):
        for x in range(8, TEX - 7, 8):
            rivet(ramps, shade, x * s + 0.5, y * s + 0.5, radius=1.5)
    # gold lock plate with a beveled edge
    for y in range(24 * s, 42 * s):
        for x in range(25 * s, 39 * s):
            ramps[y, x] = GOLD
            top, left = y - 24 * s, x - 25 * s
            bottom, right = 42 * s - 1 - y, 39 * s - 1 - x
            if min(top, left) == 0:
                v = 12
            elif min(bottom, right) == 0:
                v = 4
            elif min(top, left, bottom, right) < s:
                v = 7
            else:
                v = 10 - (DESIGN[y] - 24) * 0.15 + (brushed[y, x] - 0.5) * 0.6
            shade[y, x] = v
    # keyhole: a round top over a slot that widens downward
    kx, ky = 31.5 * s + 0.5, 29 * s + 0.5
    for y in range(26 * s, 38 * s):
        for x in range(29 * s, 35 * s):
            in_top = (x - kx) ** 2 + (y - ky) ** 2 <= (1.7 * s) ** 2
            in_slot = ky <= y <= 36.5 * s and abs(x - kx) <= 0.75 * s + (y - ky) * 0.06
            if in_top or in_slot:
                ramps[y, x] = GRAY
                shade[y, x] = 1
    return idx(ramps, np.clip(shade, 1, 15))


def rune_wall(rng=RNG, ramp=PURPLE):
    img = cave_rock(DEEP, rng, size=WALL)
    # Carve a grid of glowing glyphs: random strokes on a 3x3 lattice of
    # design points, rasterized by distance so they stay smooth.
    xs = DESIGN[None, :]
    ys = DESIGN[:, None]
    dist = np.full((WALL, WALL), np.inf)
    for gx in (12, 32, 52):
        for gy in (14, 34, 52):
            if rng.random() < 0.35:
                continue
            pts = [(gx + dx * 4, gy + dy * 4) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
            for _ in range(rng.integers(2, 5)):
                a = pts[rng.integers(0, 9)]
                b = pts[rng.integers(0, 9)]
                dist = np.minimum(dist, segment_distance(xs, ys, a, b))
    core = dist <= 0.6
    glow = (dist <= 1.5) & ~core
    img = np.where(core, idx(ramp, 13 + (dist <= 0.25)), img)
    return np.where(glow, idx(ramp, 7 - (dist - 0.6) * 2.5), img)



# ---------------------------------------------------------------------------
# Fonts
# ---------------------------------------------------------------------------
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


def cracked_wall(rng=RNG):
    """Bricks split by a jagged crack with warm light seeping through: the
    tell for a secret vault that a fireball can open."""
    img = bricks(STONE, rng=rng, base=6.5)
    ramps = img // 16
    shade = (img % 16).astype(np.float64)
    # The crack's center line has one x per design row (a jittered walk);
    # sparks of light escape beside it.
    center = {}
    sparks = []
    x = 30.0
    for y in range(4, 60):
        x += rng.uniform(-1.6, 1.6)
        x = min(max(x, 8), 56)
        center[y] = x
        if rng.random() < 0.12:
            bx = int(round(x))
            for k in range(1, rng.integers(4, 9)):
                sparks.append((bx + k * (1 if rng.random() < 0.5 else -1), y + k // 2))
    # Each fine row interpolates the center line; by distance from it, a
    # texel is the white-hot core, the gold glow (in the wide middle), the
    # dark rim, or brick lit by the light leaking out.
    for fy in range(4 * WALL_SCALE, 60 * WALL_SCALE):
        yd = min(max(DESIGN[fy], 4.0), 59.0)
        y0 = int(yd)
        y1 = min(y0 + 1, 59)
        xc = center[y0] + (center[y1] - center[y0]) * (yd - y0)
        w = 1 + (16 < round(yd) < 48)
        for fx in range(WALL):
            d = abs(DESIGN[fx] - xc)
            if d < 0.5:
                ramps[fy, fx] = FIRE
                shade[fy, fx] = 15 if d < 0.25 else 14
            elif d < w - 0.5:
                ramps[fy, fx] = GOLD
                shade[fy, fx] = 12 - (d - 0.5)
            elif d < w + 0.5:
                ramps[fy, fx] = STONE
                shade[fy, fx] = 2
            elif d < w + 2.0:
                shade[fy, fx] += 1.5 * (w + 2.0 - d) / 1.5
    s = WALL_SCALE
    for sx, sy in sparks:
        if 0 <= sx < TEX and 0 <= sy < TEX:
            ramps[sy * s, sx * s] = FIRE
            shade[sy * s, sx * s] = 11
            ramps[sy * s + 1, sx * s + 1] = FIRE
            shade[sy * s + 1, sx * s + 1] = 9
    return idx(ramps, np.clip(shade, 1, 15))


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


SPR_DESIGN = 32  # billboard sprites are laid out on a 32x32 design grid
SPR_SCALE = 2    # and rasterized this many times finer
SPR = SPR_DESIGN * SPR_SCALE  # billboard sprite size in texels


class Canvas:
    """A billboard sprite: SPR x SPR palette indices (0 = transparent).

    Shapes are placed in design units (0..32) and rasterized SPR_SCALE times
    finer. Analytic fills walk the fine pixels with pixels() and draw them with
    put(), so curves and shading gain real detail; set() fills a whole design
    pixel, for features that are meant to stay chunky (eyes, rivets)."""

    def __init__(self):
        self.px = np.zeros((SPR, SPR), dtype=np.int64)

    def set(self, x, y, ramp, shade):
        """Fill design pixel (x, y) (rounded) with one color."""
        x, y = int(round(x)), int(round(y))
        v = idx(ramp, max(1, min(15, shade)))
        for fy in range(y * SPR_SCALE, (y + 1) * SPR_SCALE):
            for fx in range(x * SPR_SCALE, (x + 1) * SPR_SCALE):
                if 0 <= fx < SPR and 0 <= fy < SPR:
                    self.px[fy, fx] = v

    @staticmethod
    def fine(x, y):
        """The fine pixel whose center is design point (x + 0.5, y + 0.5)."""
        return int(round((x + 0.5) * SPR_SCALE - 0.5)), int(round((y + 0.5) * SPR_SCALE - 0.5))

    def put(self, x, y, ramp, shade):
        """Draw the single fine pixel at design coordinates (x, y), as yielded
        by pixels() (so x + 0.5, y + 0.5 is its center)."""
        fx, fy = self.fine(x, y)
        if 0 <= fx < SPR and 0 <= fy < SPR:
            self.px[fy, fx] = idx(ramp, max(1, min(15, shade)))

    def at(self, x, y):
        """Palette index of the fine pixel at design coordinates (x, y)."""
        fx, fy = self.fine(x, y)
        return self.px[fy, fx] if 0 <= fx < SPR and 0 <= fy < SPR else 0

    @staticmethod
    def parity(x, y):
        """Checkerboard parity of the fine pixel at (x, y), for dithering."""
        fx, fy = Canvas.fine(x, y)
        return (fx + fy) % 2

    @staticmethod
    def pixels(x0=0, y0=0, x1=SPR_DESIGN, y1=SPR_DESIGN):
        """Yield the design coordinates (x, y) of every fine pixel whose
        center lies in the design rectangle [x0, x1) x [y0, y1); x + 0.5 and
        y + 0.5 are the pixel's center, as in a loop over design pixels."""
        for fy in range(int(np.floor(y0 * SPR_SCALE)), int(np.ceil(y1 * SPR_SCALE))):
            for fx in range(int(np.floor(x0 * SPR_SCALE)), int(np.ceil(x1 * SPR_SCALE))):
                yield (fx + 0.5) / SPR_SCALE - 0.5, (fy + 0.5) / SPR_SCALE - 0.5

    def outline(self, ramp=GRAY, shade=1):
        """Add a dark one-texel outline around opaque texels."""
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
    for x, y in c.pixels(cx - rx - 1, cy - ry - 1, cx + rx + 1, cy + ry + 1):
        nx = (x + 0.5 - cx) / rx
        ny = (y + 0.5 - cy) / ry
        d = nx * nx + ny * ny
        if d > 1.0:
            continue
        nz = np.sqrt(1 - d)
        diff = max(0.0, nx * lx + ny * ly + nz * lz)
        shade = base - 4 + diff * 8
        if spec and diff > 0.93:
            shade = 15
        c.put(x, y, ramp, shade)


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
    for x, y in c.pixels(0, 0, SPR_DESIGN, SPR_DESIGN):
        # distance along a wing from the body, in design pixels
        i = abs(x + 0.5 - 16) - 3.5
        if not 0 <= i < 12:
            continue
        tip = flap * (i / 11.0)
        top = 13 + tip - (3 - 2 * np.clip((i - 5) / 1.5, 0, 1)) * np.sin(i / 11.0 * np.pi)
        bottom = 18 + tip * 0.8 - (int(i) % 4 == 3) * 2 * np.sin((i % 1) * np.pi)
        if top <= y + 0.5 < bottom:
            vein = abs(((i + 1.5) % 4) - 2) < 0.25
            c.put(x, y, PURPLE, 5 + (y + 0.5 - top < 0.8) * 3 - vein * 2)
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
        for r in np.linspace(7, 10.5, 12):
            c.put(16 + np.cos(a) * r - 0.5, 15 + np.sin(a) * r - 0.5, RED, 8)
            a += rng.uniform(-0.125, 0.125)
    look = [(0, 0), (-3, 1), (3, 1), (0, 0), (0, 0)][phase % 5]
    ix, iy = 16 + look[0], 15 + look[1]
    iris_ramp = FIRE if phase == 4 else RED
    for x, y in c.pixels(ix - 6, iy - 6, ix + 6, iy + 6):
        d = np.hypot(x + 0.5 - ix, y + 0.5 - iy)
        if d < 5.5:
            # radial streaks in the iris
            streak = np.sin(np.arctan2(y + 0.5 - iy, x + 0.5 - ix) * 9) * 0.8
            c.put(x, y, iris_ramp, 6 + (5.5 - d) * 1.4 + streak)
        if d < 2.5:
            c.put(x, y, GRAY, 1)
    c.set(ix - 2, iy - 3, GRAY, 15)
    if phase == 3:
        for x, y in c.pixels(6, 5, 27, 25):
            if c.at(x, y) and abs(y + 0.5 - 15.5) > 2.5:
                c.put(x, y, BONE, 6 if y < 15 else 5)
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
    for x, y in c.pixels(0, 4, SPR_DESIGN, 31):
        half = 4 + min(y + 0.5 - 4.5, 10) * 0.8
        dx = x + 0.5 - 16
        wave = np.sin(x * 0.7 + phase * 1.5) * 2
        if abs(dx) < half and y < 28 + wave:
            shade = 11 - abs(dx) / half * 5 + (y < 10) * 2
            if Canvas.parity(x, y) == 0 and y > 20:
                shade -= 2
            c.put(x, y, ICE, shade)
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
        for x, y in c.pixels(0, by - h, SPR_DESIGN, by):
            t = (by - (y + 0.5)) / h
            half = w * (1.0 if t < 0.75 else (1 - t) / 0.25)
            cx = bx + lean * (by - (y + 0.5))
            dx = x + 0.5 - cx
            if abs(dx) <= half:
                facet = 12 if dx < -half * 0.3 else (7 if dx > half * 0.3 else 10)
                c.put(x, y, GEM, facet)
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
    for x, y in c.pixels(4, 16, 16, 28):
        d = np.hypot(x + 0.5 - 10, y + 0.5 - 22)
        if 2.2 < d < 5.2:
            c.put(x, y, GOLD, 12 - (y + 0.5 > 22) * 4 + (abs(d - 3.7) < 0.5) * 2)
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
    for x, y in c.pixels():
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
        c.put(x, y, GEM if swirl > -0.2 else ICE, shade)
    return c.px


def sprite_orb(ramp, radius=6.0):
    """Glowing spell orb: bright core fading to a soft rim."""
    c = Canvas()
    for x, y in c.pixels():
        r = np.hypot(x + 0.5 - 16, y + 0.5 - 16)
        if r < radius:
            c.put(x, y, ramp, 15 - (r / radius) * 7)
        elif r < radius + 2.5 and Canvas.parity(x, y) == 0:
            c.put(x, y, ramp, 6)
    return c.px


def sprite_puff(phase, ramp):
    """Expanding burst for impacts and deaths (phase 0..3)."""
    c = Canvas()
    rng = np.random.default_rng(7 + phase)
    radius = 4 + phase * 3.5
    for _ in range(260):
        a = rng.uniform(0, 2 * np.pi)
        r = rng.uniform(0, radius)
        shade = 15 - (r / radius) * 6 - phase * 2
        x, y = 16 + np.cos(a) * r - 0.5, 16 + np.sin(a) * r - 0.5
        c.put(x, y, ramp, shade)
        if r < radius * 0.45:
            # the hot middle of the burst is denser
            c.put(x + 0.5, y, ramp, shade - 1)
    return c.px


STAFF_SCALE = 2  # the staff is designed on a 32x64 grid and drawn at 2x detail
STAFF_W = 32 * STAFF_SCALE
STAFF_H = 64 * STAFF_SCALE


def overlay_staff():
    """The player's crystal-tipped staff, held from the lower right and leaning
    into the view: STAFF_W x STAFF_H palette indices (0 = transparent). Shapes
    are laid out in 32x64 design units and rasterized STAFF_SCALE times finer,
    so edges and shading gain detail rather than just growing."""
    S = STAFF_SCALE
    img = np.zeros((STAFF_H, STAFF_W), dtype=np.int64)

    def put(x, y, ramp, shade):
        x, y = int(np.floor(x)), int(np.floor(y))
        if 0 <= x < STAFF_W and 0 <= y < STAFF_H:
            img[y, x] = idx(ramp, max(1, min(15, shade)))

    def design(x, y):
        """Design-unit coordinates of the center of fine pixel (x, y)."""
        return (x + 0.5) / S, (y + 0.5) / S

    # shaft from the bottom right up toward the upper left: a band 5 units
    # wide around a gently wavering center line, with grain along its length
    for t in np.linspace(0, 1, 320 * S):
        cx = 27 - t * 13 + np.sin(t * 9) * 0.8
        cy = 63.5 - t * 50
        for w in np.arange(-2.5, 2.5, 1.0 / S):
            shade = 7 - w * 1.5 + np.sin(t * 40) * 0.6 + np.sin(t * 173 + w * 3) * 0.35
            put((cx + w) * S, cy * S, WOOD, shade)
    # crystal head: a faceted diamond lit from the upper left
    for y in range(0, 17 * S):
        for x in range(6 * S, 22 * S):
            u, v = design(x, y)
            dx = u - 14
            dy = v - 8
            r = abs(dx) * 1.6 + abs(dy)
            if r > 8.5:
                continue
            facet = 13 if dx < -1 else (8 if dx > 1.5 else 11)
            if r > 7.7:
                facet = 4
            elif r > 7.2:
                facet -= 2
            # a soft highlight streak across the left facet
            if -5.5 < dx < -2.5 and -4 < dy + dx * 0.4 < -1.5:
                facet = 15
            put(x, y, GEM, facet - (v > 10) * 2)
    # claw prongs holding the crystal
    for (u, v) in ((9, 14), (10, 15), (18, 14), (17, 15), (13, 16), (14, 16), (15, 16)):
        for oy in range(S):
            for ox in range(S):
                put(u * S + ox, v * S + oy, GOLD, 9 + (ox == 0 and oy == 0) * 3 - (oy == S - 1) * 3)
    # the fist: four curled fingers stacked down the shaft, the thumb wrapped
    # over the top, and the back of the hand behind them (lit from the left)
    for y in range(33 * S, 52 * S):
        for x in range(14 * S, 31 * S):
            u, v = design(x, y)
            shade = None
            for i in range(4):
                fy = 38.0 + i * 3.25
                fx0 = 16.6 + i * 0.35
                dyf = v - fy
                if not (fx0 <= u <= 26.5 and abs(dyf) <= 1.6):
                    continue
                if u - fx0 < 1.3 and (u - fx0 - 1.3) ** 2 + dyf ** 2 > 1.9:
                    continue  # rounded fingertip
                shade = 10.5 - dyf * 1.1 - (u - fx0) * 0.13
                if abs(dyf) > 1.25:
                    shade -= 3  # crease between fingers
            if shade is None:
                tdx, tdy = u - 20.2, v - 35.4
                if (tdx / 4.4) ** 2 + (tdy / 1.8) ** 2 <= 1.0:
                    shade = 11.5 - tdy * 1.1 - tdx * 0.12
            if shade is None:
                bdx, bdy = u - 26.2, v - 43.0
                if (bdx / 4.2) ** 2 + (bdy / 8.5) ** 2 <= 1.0:
                    shade = 8.5 - bdx * 0.35 - bdy * 0.1
            if shade is not None:
                put(x, y, BONE, shade)
    # sleeve
    for y in range(50 * S, 64 * S):
        for x in range(14 * S, 30 * S):
            u, v = design(x, y)
            if u - 14 > (v - 50) * 0.3:
                fold = np.sin(u * 1.3 + v * 0.4) * 0.8
                put(x, y, BLUE, 4 + (u - 14) * 0.15 + fold)
    return img


def staff_glow(img):
    """The staff while casting: the crystal blazes and sparks ring it."""
    S = STAFF_SCALE
    glow = img.copy()
    for y in range(32 * S):
        for x in range(STAFF_W):
            v = int(glow[y, x])
            if v and v // 16 == GEM:
                glow[y, x] = idx(GEM, min(15, v % 16 + 3))
    for (u, v) in ((6, 3), (22, 2), (4, 10), (24, 11), (14, 0), (8, 16), (20, 17)):
        for (ox, oy) in ((0, 0), (1, 0), (0, 1), (-1, 0), (0, -1)):
            x, y = u * S + ox, v * S + oy
            if 0 <= x < STAFF_W and 0 <= y < STAFF_H:
                glow[y, x] = idx(GRAY, 15 if (ox, oy) == (0, 0) else 11)
    return glow


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


def sprite_torch(phase):
    """Wall torch: iron bracket and a flickering flame (phase 0..3)."""
    c = Canvas()
    rng = np.random.default_rng(31 + phase)
    # bracket and handle
    for y in range(18, 31):
        c.set(15, y, WOOD, 5)
        c.set(16, y, WOOD, 7)
        c.set(17, y, WOOD, 4)
    for x in range(12, 21):
        c.set(x, 18, GRAY, 6 + (x == 12) * 4)
        c.set(x, 19, GRAY, 4)
    c.set(12, 17, GRAY, 8)
    c.set(20, 17, GRAY, 8)
    # flame: stacked shrinking blobs swaying with the phase
    sway = [0.0, 0.8, 0.0, -0.8][phase % 4]
    height = 12 + rng.integers(-1, 2)
    for x, y in c.pixels(0, 17 - height, SPR_DESIGN, 18):
        t = (17.5 - (y + 0.5)) / height
        if t < 0 or t > 1:
            continue
        half = 3.6 * (1 - t) ** 0.7 + 0.4
        cx = 16 + sway * t * 2 + np.sin(t * 6 + phase) * 0.6
        dx = abs(x + 0.5 - cx)
        if dx <= half:
            core = 1 - dx / half
            if t < 0.45 and core > 0.45:
                c.put(x, y, GOLD, 13 + core * 2)
            elif core > 0.25:
                c.put(x, y, FIRE, 10 + core * 4 - t * 3)
            else:
                c.put(x, y, RED, 9 - t * 3)
    return c.px


def sprite_warden(phase):
    """The Warden: a great eye sealed in a dioptase crystal shell, ringed by
    spikes. Phase 0/1 idle (iris drifts), 2 charging (white-hot iris)."""
    c = Canvas()
    # spikes
    for k in range(10):
        a = k * 2 * np.pi / 10 + 0.3
        for r in np.linspace(9, 15.5, 28):
            w = (15.5 - r) / 6.5 * 1.6
            for o in np.linspace(-w, w, 8):
                x = 16 + np.cos(a) * r - np.sin(a) * o
                y = 15 + np.sin(a) * r + np.cos(a) * o
                c.put(x - 0.5, y - 0.5, GEM, 6 + (r < 12) * 3 - (o > 0) * 2)
    # faceted shell
    shaded_blob(c, 16, 15, 10.5, 10.5, GEM, base=8, spec=False)
    for x, y in c.pixels(5, 4, 27, 26):
        dx, dy = x + 0.5 - 16, y + 0.5 - 15
        if dx * dx + dy * dy < 110 and (abs(dx + dy) < 0.4 or abs(dx - dy) < 0.4):
            c.put(x, y, GEM, 4)
    # eye
    look = [(-1.5, 0), (1.5, 0.5), (0, 0)][phase % 3]
    for x, y in c.pixels(8, 9, 24, 21):
        dx, dy = x + 0.5 - 16, y + 0.5 - 15
        if (dx / 7.0) ** 2 + (dy / 5.0) ** 2 <= 1.0:
            c.put(x, y, BONE, 12 - abs(dy) * 0.8)
        ix, iy = dx - look[0], dy - look[1]
        d = np.hypot(ix, iy)
        if d < 3.6:
            if phase == 2:
                c.put(x, y, GOLD if d < 2 else FIRE, 15 - d)
            else:
                c.put(x, y, RED, 11 - d * 1.5 + np.sin(np.arctan2(iy, ix) * 7) * 0.7)
        if d < 1.4 and phase != 2:
            c.put(x, y, GRAY, 1)
    c.set(14, 12, GRAY, 15)
    c.outline(DEEP, 1)
    return c.px


def sprite_bones():
    """A skull and a few scattered bones lying on the floor."""
    c = Canvas()
    # skull
    shaded_blob(c, 13, 25, 5, 4.5, BONE, base=9, spec=False)
    for x in range(10, 17):
        c.set(x, 29, BONE, 7)
    for (x, y) in ((11, 25), (12, 25), (15, 25), (14, 25)):
        c.set(x, y, GRAY, 1)
    c.set(13, 27, GRAY, 2)
    # bones
    for t in np.linspace(0, 1, 14):
        c.set(18 + t * 10, 28 - t * 3, BONE, 11)
        c.set(18 + t * 10, 29 - t * 3, BONE, 7)
    for (x, y) in ((17, 27), (17, 29), (28, 24), (28, 26)):
        c.set(x, y, BONE, 12)
    for t in np.linspace(0, 1, 9):
        c.set(4 + t * 6, 30 - t * 1, BONE, 9)
    c.outline(GRAY, 2)
    return c.px


def sprite_mushrooms():
    """Glowing cave mushrooms: three luminous caps on pale stalks."""
    c = Canvas()
    for (cx, base, h, r, ramp) in ((10, 31, 9, 5.5, ICE), (19, 31, 13, 6.5, ICE), (25, 31, 6, 3.5, PURPLE)):
        top = base - h
        for y in range(top, base):
            c.set(cx, y, BONE, 11)
            c.set(cx + 1, y, BONE, 8)
        for x, y in c.pixels(cx - r - 1, top - r, cx + r + 2, top + 1):
            dx = (x + 0.5 - cx - 0.5) / r
            dy = (y + 0.5 - top) / r
            if dx * dx + dy * dy <= 1.0 and dy <= 0.2:
                shade = 15 - (dx + 1) * 2 - (dy + 1) * 2
                # pale spots on the cap
                if ((x * 1.7 + y * 2.3) % 3.1) < 0.45 and dy < -0.25:
                    shade = 15
                c.put(x, y, ramp, shade)
        c.set(cx - 1, int(top - r * 0.5), GRAY, 15)
    c.outline(DEEP, 2)
    return c.px


SPRITES = []  # (name, SPR x SPR row-major palette indices)


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
    for p in range(4):
        SPRITES.append((f"puff_spark{p}", sprite_puff(p, ICE)))
    for p in range(4):
        SPRITES.append((f"puff_pink{p}", sprite_puff(p, PINK)))
    for p in range(4):
        SPRITES.append((f"torch{p}", sprite_torch(p)))
    for p in range(3):
        SPRITES.append((f"warden{p}", sprite_warden(p)))
    SPRITES.append(("bones", sprite_bones()))
    SPRITES.append(("mushrooms", sprite_mushrooms()))


FLATS = []  # floor/ceiling textures, row-major


TEXTURES = []  # (name, [row-major index array per mip level])


def halve(img):
    """Box-filter a palette-index texture to half size: each 2x2 block becomes
    the palette entry nearest its mean color among the ramps the block uses,
    so a mip level never brings in a hue its texture lacks."""
    n = img.shape[0] // 2
    blocks = img.reshape(n, 2, n, 2).transpose(0, 2, 1, 3).reshape(n, n, 4)
    mean = PALETTE[blocks].mean(axis=2)
    best = np.zeros((n, n), dtype=np.int64)
    best_err = np.full((n, n), np.inf)
    for k in range(4):
        candidates = (blocks[:, :, k] // 16)[:, :, None] * 16 + np.arange(16)
        err = ((PALETTE[candidates] - mean[:, :, None, :]) ** 2).sum(axis=3)
        pick = err.argmin(axis=2)[:, :, None]
        pick_err = np.take_along_axis(err, pick, 2)[:, :, 0]
        better = pick_err < best_err
        best = np.where(better, np.take_along_axis(candidates, pick, 2)[:, :, 0], best)
        best_err = np.where(better, pick_err, best_err)
    return best


def add_texture(name, img):
    """Register a WALL-texel wall texture with its smaller mip levels."""
    mips = [np.asarray(img, dtype=np.int64)]
    for _ in range(WALL_MIPS - 1):
        mips.append(halve(mips[-1]))
    TEXTURES.append((name, mips))


def build_textures():
    add_texture("brick", bricks(STONE, rng=np.random.default_rng(1)))
    add_texture("brick_moss", bricks(STONE, moss=0.55, rng=np.random.default_rng(2)))
    add_texture("rock", cave_rock(RUST, rng=np.random.default_rng(3), size=WALL))
    add_texture("rock_gem", cave_rock(DEEP, rng=np.random.default_rng(4), crystals=6, size=WALL))
    add_texture("door", wood_door(np.random.default_rng(5)))
    add_texture("door_locked", iron_door(np.random.default_rng(6)))
    add_texture("runes", rune_wall(np.random.default_rng(7)))
    add_texture("brick_dark", bricks(DEEP, rows=8, cols=2, base=6.0, rng=np.random.default_rng(8)))
    add_texture("cracked", cracked_wall(np.random.default_rng(9)))
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
    # texture sheets, every texture blown up to `size` pixels, 4 per row:
    # walls at mip level 0 then the flats, and each wall's mip levels
    size = 256
    per_row = 4

    def texture_sheet(images, name):
        rows = (len(images) + per_row - 1) // per_row
        sheet = np.zeros((rows * (size + 4), per_row * (size + 4), 3), dtype=np.uint8)
        for k, img in enumerate(images):
            r, c = divmod(k, per_row)
            scale = size // img.shape[0]
            rgb = np.repeat(np.repeat(rgb_of[img], scale, 0), scale, 1)
            sheet[r * (size + 4):r * (size + 4) + size, c * (size + 4):c * (size + 4) + size] = rgb
        write_png(os.path.join(dirname, name), sheet)

    texture_sheet([mips[0] for _, mips in TEXTURES] + [img for _, img in FLATS], "textures.png")
    texture_sheet([m for _, mips in TEXTURES for m in mips], "wall_mips.png")
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
    # the staff, resting and casting, 3x scale on the checker background
    scale = 3
    staff = overlay_staff()
    sheet = np.zeros((STAFF_H * scale, 2 * STAFF_W * scale + 8, 3), dtype=np.uint8)
    for k, img in enumerate((staff, staff_glow(staff))):
        rgb = rgb_of[img].astype(np.uint8)
        check = ((np.arange(STAFF_H)[:, None] // 8 + np.arange(STAFF_W)[None, :] // 8) % 2) * 20 + 30
        rgb = np.where((img == 0)[:, :, None], check[:, :, None].astype(np.uint8), rgb)
        rgb = np.repeat(np.repeat(rgb, scale, 0), scale, 1)
        x0 = k * (STAFF_W * scale + 8)
        sheet[:, x0:x0 + STAFF_W * scale] = rgb
    write_png(os.path.join(dirname, "staff.png"), sheet)


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
    music_data = music.MusicData(music.build_instruments(), songs.build_songs())

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
    # The texel arrays start on page boundaries so no texture column, and no
    # 4 KB flat or sprite, straddles two pages: the renderer's inner loops
    # then touch one TLB entry per column or flat (the TLB holds only 16).
    # Walls are sampled down columns, so each mip level stores its textures
    # column-major, one array per level (wall_textures is level 0).
    for level, label in enumerate(("wall_textures", "wall_mip1", "wall_mip2")[:WALL_MIPS]):
        out.append("  .align 4096")
        out.append(f"  .global {label}")
        out.append(f"{label}:")
        for name, mips in TEXTURES:
            out.append(f"  # {name}")
            emit_bytes(out, None, mips[level].T.reshape(-1))
    out.append("  .align 4096")
    # Floors and ceilings are sampled at arbitrary (u, v), stored row-major.
    out.append("  .global flat_textures")
    out.append("flat_textures:")
    for name, img in FLATS:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.reshape(-1))
    out.append("  .align 4096")
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
    # The staff, drawn into the view by draw_image_col (gfx.s): two images
    # (resting, casting), each column-major 12-bit pixels with 0 meaning
    # transparent, then each column's opaque [top, bottom) rows. Casting
    # lights the crystal at the overbright level.
    staff = overlay_staff()
    glow_full = shade_tables[LIGHT_LEVELS - 1]
    images = []
    spans = []
    for img, table in ((staff, full), (staff_glow(staff), None)):
        pix = np.zeros(img.shape, dtype=np.int64)
        for y in range(STAFF_H):
            for x in range(STAFF_W):
                v = int(img[y, x])
                if v == 0:
                    continue
                t = full
                if table is None and (v // 16 == GEM or v == idx(GRAY, 15)) and y < 32 * STAFF_SCALE:
                    t = glow_full
                pix[y, x] = max(1, int(t[v]))   # opaque black must not read as clear
        images.extend(pix.T.reshape(-1))
        for x in range(STAFF_W):
            col = np.nonzero(img[:, x])[0]
            spans.extend([int(col[0]), int(col[-1]) + 1] if len(col) else [0, 0])
    emit_halfs(out, "staff_images", images)
    emit_bytes(out, "staff_spans", spans)
    out.append("  .align 4")
    emit_halfs(out, "overlay_cursor", to_sprite_pixels(overlay_cursor(), full))
    out.append("  .align 4")
    music.emit_music(out, music_data, emit_bytes, emit_words, emit_halfs)
    out.append("  .align 4")
    here = os.path.dirname(os.path.abspath(__file__))
    rows, spans = big_font_bytes(os.path.join(here, "..", "..", "..", "common", "text_tiles.s"))
    emit_bytes(out, "font_big", rows)
    emit_bytes(out, "font_big_spans", spans)
    out.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(out))
    if preview_dir:
        preview(preview_dir, shade_tables)
    here_out = os.path.dirname(os.path.abspath(out_path))
    header = os.path.join(here_out, "music_data.h")
    with open(header, "w") as f:
        f.write(music.header_text(music_data))
    music.write_samples(os.path.join(here_out, "music.dat"), music_data)
    print(f"wrote {out_path} and {header}: {len(TEXTURES)} textures, {len(FLATS)} flats, "
          f"{len(SPRITES)} sprites, {len(music_data.inst_names)} instruments, "
          f"{len(music_data.song_words) // 4} songs")
    for i, (name, _) in enumerate(SPRITES):
        print(f"  sprite {i:2d} {name}")


if __name__ == "__main__":
    main()
