#!/usr/bin/env python3
"""Generate SKYFORGE's high-resolution art, shade tables, and music assets.

Produces:
  - assets.s     : 16x256 shade tables, 4096-entry sine LUT, quarter-square
                   multiplier table, 8 page-aligned 64x64 terrain/cliff
                   textures, 28 page-aligned 64x64 billboard sprites, 5
                   banked 80x48 player starfighter sprites, and tracker tables.
  - music.dat    : 8-bit mu-law instrument samples mapped at runtime.
  - music_data.h : C declarations and array bounds for the tracker tables.
"""
import os
import sys
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import music  # noqa: E402
import songs  # noqa: E402

RNG = np.random.default_rng(0x5F04E)
TEX = 64
SPR = 64
SHIP_W = 80
SHIP_H = 48

# 16 palette ramps x 16 shades (0..15)
GRAY, STONE, WOOD,SAND, GRASS, GEM, ICE, WATER, PURPLE, RED, FIRE, GOLD, STEEL, CYAN, PINK, NAVY = range(16)

RAMP_KEYS = [
    [(0, 0, 0), (120, 120, 125), (255, 255, 255)],       # 0 GRAY
    [(18, 16, 16), (110, 102, 94), (220, 210, 195)],     # 1 STONE
    [(28, 14, 6), (130, 76, 36), (235, 185, 125)],       # 2 WOOD
    [(35, 28, 12), (175, 150, 90), (250, 235, 180)],     # 3 SAND
    [(8, 24, 6), (48, 135, 42), (165, 235, 115)],        # 4 GRASS
    [(0, 26, 16), (0, 185, 120), (185, 255, 230)],       # 5 GEM (Dioptase)
    [(6, 20, 34), (70, 165, 215), (225, 252, 255)],      # 6 ICE
    [(4, 14, 42), (28, 95, 210), (145, 215, 255)],       # 7 WATER
    [(18, 6, 32), (130, 52, 185), (240, 185, 255)],      # 8 PURPLE
    [(32, 4, 4), (200, 28, 22), (255, 175, 155)],        # 9 RED
    [(36, 10, 0), (240, 110, 12), (255, 245, 150)],      # 10 FIRE
    [(30, 20, 0), (220, 175, 28), (255, 252, 195)],      # 11 GOLD
    [(12, 16, 24), (95, 115, 145), (220, 235, 255)],     # 12 STEEL
    [(0, 24, 30), (20, 185, 220), (190, 255, 255)],      # 13 CYAN
    [(30, 0, 18), (220, 48, 160), (255, 205, 240)],      # 14 PINK
    [(4, 8, 22), (32, 58, 125), (120, 165, 245)],        # 15 NAVY
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
LIGHT_LEVELS = 16
LIGHT_FULL = 12
FOG_COLOR = np.array([18.0, 42.0, 68.0])
OVERBRIGHT = np.array([60.0, 45.0, 15.0])


def to12(rgb):
    return np.clip(np.round(np.asarray(rgb) / 17.0), 0, 15).astype(np.int64)


def pack12(c4):
    return int(c4[0]) | (int(c4[1]) << 4) | (int(c4[2]) << 8)


def build_shade_tables():
    tables = np.zeros((LIGHT_LEVELS, 256), dtype=np.int64)
    for level in range(LIGHT_LEVELS):
        if level <= LIGHT_FULL:
            f = (level / LIGHT_FULL) ** 1.05
            for i in range(256):
                c = PALETTE[i] * f + FOG_COLOR * (1.0 - f)
                tables[level, i] = pack12(to12(c))
        else:
            extra = (level - LIGHT_FULL) / 3.0
            for i in range(256):
                c = PALETTE[i] * (1.0 + extra * 0.35) + OVERBRIGHT * extra
                tables[level, i] = pack12(to12(c))
    return tables


def idx(ramp, shade):
    s = np.clip(np.round(shade), 0, 15).astype(np.int64)
    return ramp * 16 + s


def value_noise(cells, size=TEX, rng=RNG):
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
    return total / norm


def build_terrain_textures():
    """Build 8 64x64 procedural terrain & cliff textures (column-major)."""
    y, x = np.mgrid[0:TEX, 0:TEX]
    n1 = fbm(4, 4, np.random.default_rng(101), TEX)
    n2 = fbm(4, 8, np.random.default_rng(202), TEX)

    # 0: TEX_WATER — Smooth sapphire river with sweeping horizontal wave ripples
    wave = np.sin(y * 2 * np.pi / 16 + np.sin(x * 2 * np.pi / 64) * 2.0) * 0.5 + 0.5
    sh_water = 6.0 + wave * 4.0
    tex_water = idx(WATER, sh_water)
    tex_water = np.where(wave > 0.92, idx(CYAN, 12), tex_water)

    # 1: TEX_SAND — Golden beach sand with pebbles
    sh_sand = 7 + (n1 - 0.5) * 4 + (n2 - 0.5) * 2
    tex_sand = idx(SAND, sh_sand)

    # 2: TEX_GRASS — Lush emerald grass with moss and dioptase wildflowers
    blade = np.sin(x * 2 * np.pi / 16 + y * 0.4) * 1.0
    sh_grass = 6 + (n1 - 0.5) * 5 + blade
    tex_grass = idx(GRASS, sh_grass)
    tex_grass = np.where((n2 > 0.76) & ((x + y) % 5 == 0), idx(GEM, 12), tex_grass)

    # 3: TEX_ROCK — Stratified canyon cliff face with horizontal strata & cracks
    strata = np.sin(y * 2 * np.pi / 12 + n1 * 2.5) * 2.5
    crack = np.abs(np.sin(x * 2 * np.pi / 32 + n2 * 3.0)) < 0.07
    sh_rock = 7 + strata + (n2 - 0.5) * 3
    tex_rock = idx(STONE, np.where(crack, 3, sh_rock))

    # 4: TEX_CRYSTAL — Canyon rock embedded with glowing Dioptase emerald veins
    vein = np.abs(np.sin((x + y * 0.7) * 2 * np.pi / 20 + n1 * 2.5)) < 0.18
    sh_gem = 9 + np.sin(x * 0.8 + y * 0.5) * 4
    tex_crystal = np.where(vein, idx(GEM, sh_gem), tex_rock)

    # 5: TEX_SNOW — Glacial ice and sparkling snow peaks
    sh_snow = 10 + (n1 - 0.5) * 4 + (n2 - 0.5) * 2
    tex_snow = idx(ICE, sh_snow)

    # 6: TEX_LAVA — Smoothly flowing molten magma currents
    lava_wave = np.sin(y * 2 * np.pi / 16 + np.cos(x * 2 * np.pi / 64) * 2.5) * 0.5 + 0.5
    tex_lava = np.where(lava_wave > 0.78, idx(GOLD, 13), idx(FIRE, 7 + lava_wave * 7))

    # 7: TEX_CYBER — Synthwave neon grid on dark indigo stone
    grid = ((x % 16 == 0) | (y % 16 == 0))
    tex_cyber = np.where(grid, idx(CYAN, 13), idx(PURPLE, 3 + n1 * 4))

    return [
        ("water", tex_water),
        ("sand", tex_sand),
        ("grass", tex_grass),
        ("rock", tex_rock),
        ("crystal", tex_crystal),
        ("snow", tex_snow),
        ("lava", tex_lava),
        ("cyber", tex_cyber),
    ]


def make_ring_sprite(frame, ramp_outer=GOLD, ramp_inner=GEM):
    """64x64 3D faceted octagonal Power Ring with sparkling inner rim."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    y, x = np.mgrid[0:SPR, 0:SPR]
    dx = x - 31.5
    dy = y - 31.5
    r = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)
    ring_mask = (r >= 19.0) & (r <= 28.5)
    bevel = 1.0 - np.abs(r - 23.7) / 4.8
    light = np.cos(ang + frame * np.pi / 2) * 3.0
    sh = np.clip(8 + bevel * 5 + light, 3, 15)
    img[ring_mask] = idx(ramp_outer, sh[ring_mask])
    inner_glow = (r >= 17.0) & (r < 19.0)
    img[inner_glow] = idx(ramp_inner, 13)
    # 4 cardinal gem facets on the ring
    for a in (0, np.pi / 2, np.pi, 3 * np.pi / 2):
        gx = 31.5 + 23.7 * np.cos(a + frame * 0.2)
        gy = 31.5 + 23.7 * np.sin(a + frame * 0.2)
        gem_m = np.hypot(x - gx, y - gy) <= 3.8
        img[gem_m] = idx(ramp_inner, 14)
    return img


def make_gate_sprite(active, frame):
    """64x64 Grand Prix Neon Gate with twin towers and holographic chevron arch."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    ramp = CYAN if active else GOLD
    # Left and right armored pylons
    for y in range(8, 62):
        for side_x in (6, 52):
            for dx in range(6):
                sh = 5 + (3 - abs(dx - 2)) * 2
                img[y, side_x + dx] = idx(STEEL, sh)
            if (y + frame * 3) % 8 < 4:
                img[y, side_x + 2] = idx(ramp, 13)
                img[y, side_x + 3] = idx(ramp, 15)
    # Top holographic energy arch
    for x in range(6, 58):
        for y in range(6, 15):
            if y in (6, 14):
                img[y, x] = idx(STEEL, 8)
            else:
                chev = ((x + y + frame * 4) % 8) < 4
                img[y, x] = idx(ramp, 14 if chev else 9)
    return img


def make_arch_sprite():
    """64x64 Carved Canyon Stone Archway with glowing Dioptase keystone."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    y, x = np.mgrid[0:SPR, 0:SPR]
    dx = x - 31.5
    dy = y - 34.0
    arch_r = np.hypot(dx * 1.1, np.maximum(0, -dy) * 1.3)
    for py in range(4, 64):
        for px in range(2, 62):
            adx = abs(px - 31.5)
            if py >= 26:
                if 18 <= adx <= 29:
                    brick = ((py // 5) + (px // 6)) % 2
                    sh = 6 + brick * 2 + (29 - adx) * 0.4
                    img[py, px] = idx(STONE, sh)
            else:
                r = np.hypot((px - 31.5) * 1.05, (py - 26.0) * 1.35)
                if 18.0 <= r <= 30.0:
                    sh = 7 + (26 - py) * 0.25
                    img[py, px] = idx(STONE, sh)
                    if abs(r - 24.0) < 1.8 and adx < 6:
                        img[py, px] = idx(GEM, 13)
    return img


def make_generator_sprite(frame):
    """64x64 Armored Canyon Shield Tower / Energy Generator."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    for y in range(10, 62):
        half_w = 14 if y > 46 else (10 if y > 22 else 7)
        for x in range(32 - half_w, 32 + half_w):
            dx = x - 32
            sh = 10 - abs(dx) * 0.5
            img[y, x] = idx(STEEL, sh)
            if 24 <= y <= 44 and abs(dx) <= 5:
                coil = ((y + frame * 2) % 6) < 3
                img[y, x] = idx(CYAN if coil else GEM, 14 if coil else 9)
    # Central vertical energy beam
    for y in range(0, 14):
        for x in range(29, 35):
            img[y, x] = idx(CYAN, 15 if abs(x - 31.5) < 1.5 else 11)
    return img


def make_portal_sprite(frame):
    """64x64 Swirling Emerald Dioptase Warp Portal."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    y, x = np.mgrid[0:SPR, 0:SPR]
    dx = x - 31.5
    dy = (y - 31.5) * 0.85
    r = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)
    for py in range(SPR):
        for px in range(SPR):
            rv = r[py, px]
            if rv > 27.0:
                continue
            if rv >= 22.5:
                img[py, px] = idx(GOLD, 8 + (27.0 - rv) * 1.5)
            else:
                swirl = np.sin(ang[py, px] * 3 + rv * 0.45 - frame * np.pi / 2)
                ramp = GEM if swirl > -0.2 else CYAN
                sh = np.clip(15 - rv * 0.35 + swirl * 3.0, 4, 15)
                img[py, px] = idx(ramp, sh)
    return img


def make_fighter_sprite(kind, frame):
    """64x64 Enemy Fighter (Scout, Interceptor, Stealth, or Rival Ace)."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    if kind == "scout":
        hull_r, wing_r, glow_r = RED, STONE, FIRE
    elif kind == "interceptor":
        hull_r, wing_r, glow_r = FIRE, STEEL, GOLD
    elif kind == "stealth":
        hull_r, wing_r, glow_r = PURPLE, NAVY, CYAN
    else:
        hull_r, wing_r, glow_r = GOLD, STEEL, CYAN

    # Swept delta wings (y = 24..40)
    for y in range(22, 42):
        t = 1.0 - abs(y - 32) / 11.0
        span = int(28 * t)
        for dx in range(-span, span + 1):
            x = 32 + dx
            if 0 <= x < SPR:
                sh = 6 + (1.0 - abs(dx) / max(1, span)) * 6
                img[y, x] = idx(wing_r, sh)
                if abs(y - 32) <= 2 and abs(dx) > 12:
                    img[y, x] = idx(hull_r, 11)
    # Central armored pod & twin vertical fins
    for y in range(16, 46):
        for dx in range(-9, 10):
            x = 32 + dx
            if (dx / 9.0) ** 2 + ((y - 32) / 13.0) ** 2 <= 1.0:
                sh = 12 - abs(dx) * 0.6 - abs(y - 28) * 0.25
                img[y, x] = idx(hull_r, sh)
    # Twin vertical stabilizer fins
    for y in range(14, 28):
        for side in (-12, 12):
            for dx in range(-2, 3):
                img[y, 32 + side + dx] = idx(hull_r, 10 - abs(dx) * 2)
    # Glowing twin thruster cores (pulsing with frame)
    thr_r = 4 + (frame & 1)
    for y in range(28, 37):
        for x in range(22, 43):
            for cx in (26, 38):
                d = np.hypot(x - cx, y - 32.5)
                if d <= thr_r:
                    img[y, x] = idx(glow_r, 15 if d < 2.0 else 11)
    return img


def make_turret_sprite():
    """64x64 Armored AA Plasma Turret on a reinforced bunker base."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    # Bunker base (y = 40..62)
    for y in range(40, 62):
        w = 18 + (y - 40) // 2
        for dx in range(-w, w + 1):
            img[y, 32 + dx] = idx(STONE, 8 - abs(dx) * 0.2)
    # Rotating turret dome (y = 24..42)
    for y in range(24, 42):
        for dx in range(-14, 15):
            if (dx / 14.0) ** 2 + ((y - 38) / 14.0) ** 2 <= 1.0:
                img[y, 32 + dx] = idx(STEEL, 11 - abs(dx) * 0.4)
    # Twin plasma cannon barrels
    for y in range(10, 30):
        for side in (-7, 7):
            for dx in range(-2, 3):
                img[y, 32 + side + dx] = idx(STEEL, 12 - abs(dx) * 2)
            if y < 14:
                img[y, 32 + side] = idx(FIRE, 14)
    return img


def make_boss_sprite(frame):
    """64x64 Leviathan Dreadnought airborne fortress."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    # Massive armored wings (y = 20..46, x = 2..62)
    for y in range(18, 48):
        span = 30 - abs(y - 33)
        for dx in range(-span, span + 1):
            x = 32 + dx
            if 0 <= x < SPR:
                plate = ((x // 6) + (y // 5)) % 2
                sh = 7 + plate * 2 - abs(dx) * 0.1
                img[y, x] = idx(STEEL, sh)
                if abs(y - 33) <= 2:
                    img[y, x] = idx(RED, 11)
    # Command citadel tower (y = 8..54)
    for y in range(8, 54):
        w = 12 - abs(y - 31) // 3
        for dx in range(-w, w + 1):
            img[y, 32 + dx] = idx(PURPLE, 10 - abs(dx) * 0.4)
    # Glowing central Leviathan reactor eye
    for y in range(24, 42):
        for x in range(24, 41):
            d = np.hypot(x - 32, y - 33)
            if d <= 7.5:
                img[y, x] = idx(FIRE if frame == 0 else GOLD, 15 if d < 3.5 else 11)
    return img


def make_orb_sprite(ramp_outer, ramp_core):
    """64x64 glowing energy projectile / plasma orb."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    y, x = np.mgrid[0:SPR, 0:SPR]
    d = np.hypot(x - 31.5, y - 31.5)
    mask = d <= 16.0
    sh = np.clip(15 - d * 0.6, 5, 15)
    img[mask] = idx(ramp_outer, sh[mask])
    core = d <= 7.0
    img[core] = idx(ramp_core, 15)
    return img


def make_explode_sprite(frame):
    """64x64 volumetric fiery explosion cloud."""
    img = np.zeros((SPR, SPR), dtype=np.int64)
    rng = np.random.default_rng(700 + frame)
    blobs = 14
    for _ in range(blobs):
        bx = 31.5 + rng.uniform(-12 - frame * 4, 12 + frame * 4)
        by = 31.5 + rng.uniform(-12 - frame * 4, 12 + frame * 4)
        br = rng.uniform(7, 13)
        for y in range(max(0, int(by - br)), min(SPR, int(by + br + 1))):
            for x in range(max(0, int(bx - br)), min(SPR, int(bx + br + 1))):
                d = np.hypot(x - bx, y - by)
                if d <= br:
                    t = 1.0 - d / br
                    ramp = GOLD if t > 0.65 else (FIRE if t > 0.3 else STONE)
                    sh = max(3, min(15, int(6 + t * 9 - frame * 2)))
                    if img[y, x] == 0 or t > 0.5:
                        img[y, x] = idx(ramp, sh)
    return img


def build_sprites():
    sprites = []
    for f in range(4):
        sprites.append((f"ring_{f}", make_ring_sprite(f, GOLD, GEM)))
    for f in range(2):
        sprites.append((f"gate_active_{f}", make_gate_sprite(True, f)))
    sprites.append(("gate_next", make_gate_sprite(False, 0)))
    sprites.append(("arch", make_arch_sprite()))
    for f in range(2):
        sprites.append((f"generator_{f}", make_generator_sprite(f)))
    for f in range(4):
        sprites.append((f"portal_{f}", make_portal_sprite(f)))
    for f in range(2):
        sprites.append((f"scout_{f}", make_fighter_sprite("scout", f)))
    for f in range(2):
        sprites.append((f"interceptor_{f}", make_fighter_sprite("interceptor", f)))
    for f in range(2):
        sprites.append((f"stealth_{f}", make_fighter_sprite("stealth", f)))
    sprites.append(("turret", make_turret_sprite()))
    for f in range(2):
        sprites.append((f"boss_{f}", make_boss_sprite(f)))
    sprites.append(("orb_player", make_orb_sprite(CYAN, GEM)))
    sprites.append(("orb_seismic", make_orb_sprite(FIRE, GOLD)))
    sprites.append(("orb_enemy", make_orb_sprite(RED, PINK)))
    for f in range(2):
        sprites.append((f"explode_{f}", make_explode_sprite(f)))
    return sprites


def make_player_ship_frame(bank_level, full_table):
    """Render an 80x48 12-bit pixel-art Aether-Interceptor at bank_level (-2..+2)."""
    img = np.zeros((SHIP_H, SHIP_W), dtype=np.int64)
    cx, cy = SHIP_W / 2.0, SHIP_H / 2.0 + 3.0
    ang = bank_level * 0.22
    ca, sa = np.cos(ang), np.sin(ang)

    for py in range(SHIP_H):
        for px in range(SHIP_W):
            dx = px - cx
            dy = py - cy
            # Rotate into ship-local coordinates (u, v)
            u = dx * ca + dy * sa
            v = -dx * sa + dy * ca
            au = abs(u)

            pix_idx = 0
            # 1. Swept outer delta wings (u in -36..+36, v in -6..+10)
            wing_v_front = -8.0 + au * 0.42
            wing_v_back = 7.0 + au * 0.12
            if au <= 35.0 and wing_v_front <= v <= wing_v_back:
                light_side = 2.0 if (u * bank_level >= 0) else -1.5
                if au >= 12.0:
                    sh = 8 + (1.0 - (v - wing_v_front) / max(1.0, wing_v_back - wing_v_front)) * 5 + light_side
                    pix_idx = idx(NAVY, sh)
                    # Emerald dioptase leading-edge trim
                    if v - wing_v_front < 1.8:
                        pix_idx = idx(GEM, 13)
                    elif abs(au - 33.0) < 1.8:
                        pix_idx = idx(STEEL, 14)
                else:
                    # Central silver-white armored fuselage
                    sh = 11 + (1.0 - au / 12.0) * 4 - (v + 10) * 0.15
                    pix_idx = idx(STEEL, sh)

            # 2. Forward nose cone & tinted glass canopy (v in -16..-2, au <= 6)
            nose_half = max(0.0, (v + 16.0) * 0.48)
            if -16.0 <= v <= 6.0 and au <= nose_half:
                if -10.0 <= v <= -1.0 and au <= nose_half * 0.55:
                    pix_idx = idx(CYAN, 13 - abs(u) * 0.8)
                else:
                    pix_idx = idx(STEEL, 13 - abs(u) * 0.5)

            # 3. Twin dorsal stabilizer fins at u = +-9
            for fin_u in (-9.0, 9.0):
                if abs(u - fin_u) <= 2.0 and -7.0 <= v <= 6.0:
                    pix_idx = idx(GEM if v < -3.0 else NAVY, 11)

            # 4. Twin plasma thruster nozzles & afterburner cores at u = +-5, v in 5..12
            for thr_u in (-5.0, 5.0):
                d_thr = np.hypot(u - thr_u, (v - 8.5) * 1.1)
                if d_thr <= 4.2:
                    if d_thr <= 1.8:
                        pix_idx = idx(GRAY, 15)
                    elif d_thr <= 3.0:
                        pix_idx = idx(GOLD, 14)
                    else:
                        pix_idx = idx(FIRE, 11)

            if pix_idx > 0:
                img[py, px] = max(1, int(full_table[pix_idx]))
    return img


def emit_words(out, values):
    for w in values:
        w = int(w) & 0xFFFFFFFF
        if w >= 0x80000000:
            w -= 1 << 32
        out.append(f"  .fill {w}")


def emit_halfs(out, label, data):
    if label:
        out.append(f"  .global {label}")
        out.append(f"{label}:")
    data = [int(v) & 0xFFFF for v in data]
    if len(data) % 2:
        data.append(0)
    emit_words(out, [data[i] | (data[i + 1] << 16) for i in range(0, len(data), 2)])


def emit_bytes(out, label, data):
    if label:
        out.append(f"  .global {label}")
        out.append(f"{label}:")
    b = [int(v) & 0xFF for v in data]
    while len(b) % 4:
        b.append(0)
    words = [b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)
             for i in range(0, len(b), 4)]
    emit_words(out, words)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "assets.s"
    shade_tables = build_shade_tables()
    textures = build_terrain_textures()
    sprites = build_sprites()
    music_data = music.MusicData(music.build_instruments(), songs.build_songs())

    out = [
        "# Generated by tools/gen_assets.py -- do not edit by hand.",
        "  .rodata",
        "  .align 4",
    ]
    emit_halfs(out, "shade_tables", shade_tables.reshape(-1))
    out.append("  .align 4")

    # 1024-entry 16.16 sine table
    sines = [int(round(np.sin(2 * np.pi * a / 1024.0) * 65536)) for a in range(1024)]
    out.append("  .global sin_lut")
    out.append("sin_lut:")
    emit_words(out, sines)

    # Quarter-square multiplier table for umul16 in gfx.s
    q = [(n * n) // 4 for n in range(-510, 511)]
    emit_halfs(out, "qsq_table", q[:510])
    emit_halfs(out, "qsq_center", q[510:])

    # Page-aligned 64x64 column-major terrain textures
    out.append("  .align 4096")
    out.append("  .global terrain_textures")
    out.append("terrain_textures:")
    for name, img in textures:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.T.reshape(-1))

    # Page-aligned 64x64 column-major billboard sprites
    out.append("  .align 4096")
    out.append("  .global sprite_texels")
    out.append("sprite_texels:")
    spans = []
    for name, img in sprites:
        out.append(f"  # {name}")
        emit_bytes(out, None, img.T.reshape(-1))
        for x in range(SPR):
            col = np.nonzero(img[:, x])[0]
            spans.extend([int(col[0]), int(col[-1]) + 1] if len(col) else [0, 0])
    emit_bytes(out, "sprite_spans", spans)

    # 5 banked 80x48 player starfighter frames (column-major 12-bit pixels)
    out.append("  .align 4")
    full = shade_tables[LIGHT_FULL]
    ship_imgs = []
    ship_sp = []
    for bank in (-2, -1, 0, 1, 2):
        f_img = make_player_ship_frame(bank, full)
        ship_imgs.extend(f_img.T.reshape(-1))
        for x in range(SHIP_W):
            col = np.nonzero(f_img[:, x])[0]
            ship_sp.extend([int(col[0]), int(col[-1]) + 1] if len(col) else [0, 0])
    emit_halfs(out, "ship_images", ship_imgs)
    emit_bytes(out, "ship_spans", ship_sp)

    # Music tracker tables
    out.append("  .align 4")
    music.emit_music(out, music_data, emit_bytes, emit_words, emit_halfs)
    out.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(out))

    here_out = os.path.dirname(os.path.abspath(out_path))
    header = os.path.join(here_out, "music_data.h")
    with open(header, "w") as f:
        f.write(music.header_text(music_data))
    music.write_samples(os.path.join(here_out, "music.dat"), music_data)
    print(f"Generated {out_path}, music_data.h, music.dat: "
          f"{len(textures)} textures, {len(sprites)} sprites, "
          f"{len(music_data.inst_names)} instruments, "
          f"{len(music_data.song_words) // 4} songs ({len(music_data.samples)} sample bytes)")


if __name__ == "__main__":
    main()
