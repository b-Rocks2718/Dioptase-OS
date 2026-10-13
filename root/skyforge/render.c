#include "skyforge.h"

/*
 * Full 320x192 single-pixel 3D Voxel-Space terrain renderer with bilinear
 * height interpolation, 64x64 procedural surface & cliff texturing, 16-level
 * distance & sun-slope shading, 64x64 depth-sorted billboard sprites, and
 * 5-frame banked 80x48 Aether-Interceptor rendering.
 */

/* Per-column mountain silhouette top row at 16 depth buckets (320 * 16). */
static unsigned char occl_top[5120];

/* Raymarching distance (in 8.8 cells) and height projection scale for 48 steps. */
static int step_dist_q8[48];
static int step_scale[48];

/* Address of single pixel (x, y) in the TLB-shaped 320x192 backbuffer. */
static unsigned short* view_at(int x, int y){
  int g = x >> 3;
  unsigned short* p = g_view + (g << 9) + (g << 8) + (x & 7);
  if (y >= VIEW_HALF_H){
    return p + HALF_SHORTS + ((y - VIEW_HALF_H) << 3);
  }
  return p + (y << 3);
}

/* Plot one 12-bit pixel at (x, y) in the 320x192 backbuffer. */
static void put_px(int x, int y, unsigned short col){
  if ((unsigned)x >= VIEW_W || (unsigned)y >= VIEW_H) return;
  *view_at(x, y) = col;
}

/* Draw a 2D line in single-pixel 320x192 view coordinates. */
static void draw_line_px(int x0, int y0, int x1, int y1, unsigned short col){
  int dx = x1 - x0;
  int dy = y1 - y0;
  int sx = dx >= 0 ? 1 : -1;
  int sy = dy >= 0 ? 1 : -1;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  int err = (dx > dy ? dx : -dy) >> 1;
  for (int steps = 0; steps < 384; ++steps){
    put_px(x0, y0, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err;
    if (e2 > -dx){ err -= dy; x0 += sx; }
    if (e2 <  dy){ err += dx; y0 += sy; }
  }
}

#ifdef SKYFORGE_HOST_TEST
/* Host C equivalents of gfx.s span drawers for deterministic host unit tests. */
void draw_voxel_span(unsigned short* dst, int count, unsigned char* texcol,
                     int texpos, int texstep, unsigned short* shade, int half_left){
  for (int i = 0; i < count; ++i){
    int t = (texpos >> 16) & 63;
    *dst = shade[texcol[t]];
    dst += 8;
    texpos += texstep;
    half_left -= 1;
    if (half_left == 0){
      dst += 29952; /* HALF_SKIP / 2 shorts */
      half_left = 96;
    }
  }
}

void draw_sprite_col(unsigned short* dst, int count, unsigned char* texcol,
                     int texpos, int texstep, unsigned short* shade, int half_left){
  for (int i = 0; i < count; ++i){
    int t = (texpos >> 16) & 63;
    unsigned char idx = texcol[t];
    if (idx != 0) *dst = shade[idx];
    dst += 8;
    texpos += texstep;
    half_left -= 1;
    if (half_left == 0){
      dst += 29952;
      half_left = 96;
    }
  }
}

void draw_image_col(unsigned short* dst, int count, unsigned short* src, int half_left){
  for (int i = 0; i < count; ++i){
    unsigned short c = *src++;
    if (c != 0) *dst = c;
    dst += 8;
    half_left -= 1;
    if (half_left == 0){
      dst += 29952;
      half_left = 96;
    }
  }
}
#endif

/* Initialize raymarching distance and perspective scale tables. */
void render_init(void){
  int dist_q8 = 320; /* Start 1.25 cells in front of camera */
  for (int s = 0; s < VOXEL_STEPS; ++s){
    step_dist_q8[s] = dist_q8;
    step_scale[s] = (180 * 256) / dist_q8;
    if (s < 18){
      dist_q8 += 128; /* 0.50 cells per step near camera for smooth foreground */
    } else if (s < 34){
      dist_q8 += 224; /* 0.875 cells per step mid-range */
    } else {
      dist_q8 += 352; /* 1.375 cells per step far range */
    }
  }
}

/* Select the 64x64 procedural terrain texture (0..7) for biome, height, and light. */
static int select_terrain_tex(int biome, int h, int light){
  if (light == 3 && h > WATER_LEVEL){
    return (biome == BIOME_OBSIDIAN || biome == BIOME_LEVIATHAN) ? TEX_LAVA : TEX_CRYSTAL;
  }
  if (h <= WATER_LEVEL){
    if (biome == BIOME_OBSIDIAN || biome == BIOME_LEVIATHAN) return TEX_LAVA;
    if (biome == BIOME_CYBER) return TEX_CYBER;
    return TEX_WATER;
  }
  if (biome == BIOME_CYBER){
    return (h > 34) ? TEX_CRYSTAL : TEX_CYBER;
  }
  if (biome == BIOME_OBSIDIAN || biome == BIOME_LEVIATHAN){
    return (h <= 14) ? TEX_LAVA : ((h >= 36) ? TEX_CRYSTAL : TEX_ROCK);
  }
  if (biome == BIOME_CRYO){
    return (h >= 24) ? TEX_SNOW : TEX_ROCK;
  }
  if (h <= WATER_LEVEL + 3) return TEX_SAND;
  if (h <= 28) return TEX_GRASS;
  if (h <= 46) return TEX_ROCK;
  return TEX_SNOW;
}

/* Project a 3D world point (wx, wy, wz) into single-pixel view coordinates
 * (out_x: 0..319, out_y: 0..191, out_step: 0..47, out_scale). */
static bool project_point3d(struct Camera* cam, int wx, int wy, int wz,
                            int* out_x, int* out_y, int* out_step, int* out_scale){
  int dx = wx - cam->x;
  int dy = wy - cam->y;
  int half_map = 64 << FIX_SHIFT;
  int full_map = 128 << FIX_SHIFT;
  while (dx > half_map) dx -= full_map;
  while (dx < -half_map) dx += full_map;
  while (dy > half_map) dy -= full_map;
  while (dy < -half_map) dy += full_map;

  int dir_x = icos(cam->angle);
  int dir_y = isin(cam->angle);
  int right_x = -dir_y;
  int right_y = dir_x;

  int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
  if (fwd < (2 << FIX_SHIFT) || fwd > (50 << FIX_SHIFT)) return false;
  int side = fixmul(dx, right_x) + fixmul(dy, right_y);

  int dist_q8 = fwd >> 8;
  if (dist_q8 < 256) dist_q8 = 256;

  int sx = (VIEW_W >> 1) + (((side >> 8) * 192) / dist_q8);
  int horizon_y = (VIEW_H >> 1) + 4 + cam->pitch;
  int roll_off = ((sx - (VIEW_W >> 1)) * cam->roll) >> 7;
  int scale = (180 * 256) / dist_q8;
  int dh_q8 = (cam->z - wz) >> 8;
  int sy = horizon_y + (((dh_q8 * scale) >> 11)) + roll_off;

  int step = (dist_q8 - 320) / 210;
  if (step < 0) step = 0;
  if (step >= VOXEL_STEPS) step = VOXEL_STEPS - 1;

  *out_x = sx;
  *out_y = sy;
  *out_step = step;
  *out_scale = scale;
  return true;
}

/* Render a 64x64 billboard sprite centered at (cx, cy) with pixel size `size_px`,
 * occluded per column against the mountain silhouette at `bucket`. */
static void draw_billboard_sprite(int cx, int cy, int size_px, int spr_frame,
                                  int shade_level, int bucket){
  if (size_px < 2) return;
  if (size_px > 160) size_px = 160;
  if (spr_frame < 0 || spr_frame >= SPR_COUNT) return;
  if (bucket < 0) bucket = 0;
  if (bucket >= OCCL_BUCKETS) bucket = OCCL_BUCKETS - 1;
  if (shade_level < 0) shade_level = 0;
  if (shade_level >= LIGHT_LEVELS) shade_level = LIGHT_LEVELS - 1;

  int half = size_px >> 1;
  int x0 = cx - half;
  int x1 = x0 + size_px;
  int top0 = cy - half;
  int bot0 = top0 + size_px;
  if (x1 <= 0 || x0 >= VIEW_W || bot0 <= 0 || top0 >= VIEW_H) return;

  int tex_step = (SPR_SIZE << 16) / size_px;
  int start_x = x0 < 0 ? 0 : x0;
  int end_x = x1 > VIEW_W ? VIEW_W : x1;
  unsigned char* spr_base = sprite_texels + (spr_frame << 12);
  unsigned char* spans = sprite_spans + (spr_frame << 7);
  unsigned short* shade = shade_tables + (shade_level << 8);

  for (int x = start_x; x < end_x; ++x){
    int tx = ((x - x0) * tex_step) >> 16;
    if ((unsigned)tx >= SPR_SIZE) continue;
    int sp_top = spans[tx << 1];
    int sp_bot = spans[(tx << 1) + 1];
    if (sp_bot <= sp_top) continue;

    int col_top = top0 + ((sp_top * size_px) >> 6);
    int col_bot = top0 + ((sp_bot * size_px) >> 6);
    int max_y = (int)occl_top[(x << 4) | bucket];
    if (col_bot > max_y) col_bot = max_y;
    if (col_bot > VIEW_H) col_bot = VIEW_H;

    int ty_pos = sp_top << 16;
    if (col_top < 0){
      ty_pos += (-col_top) * tex_step;
      col_top = 0;
    }
    if (col_bot <= col_top) continue;

    draw_sprite_col(view_at(x, col_top), col_bot - col_top,
                    spr_base + (tx << 6), ty_pos, tex_step,
                    shade, HALF_LEFT(col_top));
  }
}

/* Draw the 3D Terrain Forge brush circle projected onto the terrain surface. */
static void draw_forge_cursor_ring(struct Camera* cam){
  int cx0 = g_player.forge_cursor_x;
  int cy0 = g_player.forge_cursor_y;
  int rad = g_player.forge_radius;
  unsigned short ring_col = (g_frame & 4) ? 0x0FF2 : 0x02FF;

  int prev_sx = -1, prev_sy = -1;
  for (int a = 0; a <= ANGLE_COUNT; a += 64){
    int wx = ((cx0 << FIX_SHIFT) + FIX_HALF) + (icos(a) * rad);
    int wy = ((cy0 << FIX_SHIFT) + FIX_HALF) + (isin(a) * rad);
    int wz = terrain_height_at(wx, wy) + (1 << FIX_SHIFT);
    int sx = 0, sy = 0, sstep = 0, sscale = 0;
    if (project_point3d(cam, wx, wy, wz, &sx, &sy, &sstep, &sscale)){
      if (prev_sx >= 0){
        draw_line_px(prev_sx, prev_sy, sx, sy, ring_col);
        draw_line_px(prev_sx, prev_sy + 1, sx, sy + 1, 0x0FFF);
      }
      prev_sx = sx;
      prev_sy = sy;
    } else {
      prev_sx = -1;
    }
  }
}

/* Draw one 3D entity using its 64x64 pixel-art billboard sprite and effects. */
static void draw_entity_3d(struct Camera* cam, struct Entity* e){
  int sx = 0, sy = 0, step = 0, scale = 0;
  if (!project_point3d(cam, e->x, e->y, e->z, &sx, &sy, &step, &scale)) return;

  int bucket = step >> 2;
  int shade_lvl = LIGHT_FULL - (step >> 2);
  if (e->flash > 0) shade_lvl = LIGHT_LEVELS - 1;
  int anim = (int)(g_frame >> 2);

  int spr = SPR_SCOUT;
  int size_px = (scale * 5) >> 2;

  if (e->kind == ENT_RING){
    spr = SPR_RING + (anim & 3);
    size_px = (scale * 7) >> 2;
    shade_lvl = LIGHT_FULL + 1;
  } else if (e->kind == ENT_GATE){
    bool is_next = (e->phase == g_player.next_gate);
    spr = is_next ? (SPR_GATE_ACTIVE + (anim & 1)) : SPR_GATE_NEXT;
    size_px = (scale * 8) >> 2;
    shade_lvl = is_next ? (LIGHT_FULL + 2) : LIGHT_FULL;
  } else if (e->kind == ENT_ARCH){
    spr = SPR_ARCH;
    size_px = scale * 3;
  } else if (e->kind == ENT_GENERATOR){
    spr = SPR_GENERATOR + (anim & 1);
    size_px = (scale * 7) >> 2;
    shade_lvl = LIGHT_FULL + 1;
    /* Draw a tall glowing energy beacon beam rising into the sky so the
     * player can spot Shield Towers from anywhere down the canyon! */
    if (sx >= 1 && sx < VIEW_W - 1){
      int beam_top = sy - size_px * 2;
      if (beam_top < 0) beam_top = 0;
      int max_y = (int)occl_top[(sx << 4) | bucket];
      int beam_bot = sy < max_y ? sy : max_y;
      for (int by = beam_top; by < beam_bot; ++by){
        put_px(sx - 1, by, 0x09F3);
        put_px(sx,     by, 0x0FFF);
        put_px(sx + 1, by, 0x09F3);
      }
    }
  } else if (e->kind == ENT_PORTAL){
    spr = SPR_PORTAL + (anim & 3);
    size_px = (scale * 9) >> 2;
    shade_lvl = LIGHT_LEVELS - 1;
    /* Vertical emerald portal beacon pillar */
    if (sx >= 1 && sx < VIEW_W - 1){
      for (int by = 0; by < sy && by < (int)occl_top[(sx << 4) | bucket]; ++by){
        put_px(sx, by, (by & 2) ? 0x0BF4 : 0x0FFF);
      }
    }
  } else if (e->kind == ENT_SCOUT){
    spr = SPR_SCOUT + (anim & 1);
    size_px = (scale * 5) >> 2;
  } else if (e->kind == ENT_INTERCEPTOR){
    spr = SPR_INTERCEPTOR + (anim & 1);
    size_px = (scale * 5) >> 2;
  } else if (e->kind == ENT_STEALTH){
    spr = SPR_STEALTH + (anim & 1);
    size_px = (scale * 5) >> 2;
  } else if (e->kind == ENT_TURRET){
    spr = SPR_TURRET;
    size_px = (scale * 5) >> 2;
  } else if (e->kind == ENT_RIVAL){
    spr = SPR_INTERCEPTOR + (anim & 1);
    size_px = (scale * 5) >> 2;
    shade_lvl = LIGHT_FULL + 2;
  } else if (e->kind == ENT_EXPLOSION){
    spr = SPR_EXPLODE + (e->phase & 1);
    size_px = (scale * 6) >> 2;
    shade_lvl = LIGHT_LEVELS - 1;
  } else if (e->kind == ENT_BOSS){
    spr = SPR_BOSS + (anim & 1);
    size_px = scale * 3;
    /* Draw orbiting shield generator pylons around the Boss */
    for (int g = 0; g < 4; ++g){
      if (!(e->shield_gen & (1 << g))) continue;
      int ang = ((int)g_frame << 3) + (g << 8);
      int px = sx + ((icos(ang) * (size_px >> 1)) >> FIX_SHIFT);
      int py = sy + ((isin(ang) * (size_px >> 2)) >> FIX_SHIFT);
      draw_billboard_sprite(px, py, size_px >> 2, SPR_GENERATOR + (anim & 1),
                            LIGHT_FULL + 2, bucket);
    }
  }

  draw_billboard_sprite(sx, sy, size_px, spr, shade_lvl, bucket);

  /* Draw overhead health bar for damaged or locked-on targets and the Boss */
  bool is_locked = (g_player.lock_target >= 0 && &g_entities[g_player.lock_target] == e);
  if ((e->kind == ENT_BOSS || is_locked || e->hp < e->max_hp) &&
      (e->kind == ENT_GENERATOR || (e->kind >= ENT_SCOUT && e->kind <= ENT_BOSS)) &&
      e->max_hp > 0 && sy - (size_px >> 1) - 5 >= 4){
    int bar_w = (e->kind == ENT_BOSS) ? 48 : 20;
    int fill_w = (e->hp * bar_w) / e->max_hp;
    int bx0 = sx - (bar_w >> 1);
    int by0 = sy - (size_px >> 1) - 5;
    unsigned short bar_col = (e->kind == ENT_GENERATOR) ? 0x0FF2 : 0x024F;
    for (int dx = 0; dx < bar_w; ++dx){
      int px = bx0 + dx;
      if ((unsigned)px < VIEW_W && by0 < (int)occl_top[(px << 4) | bucket]){
        unsigned short c = (dx < fill_w) ? bar_col : 0x0222;
        put_px(px, by0, c);
        put_px(px, by0 + 1, c);
      }
    }
  }
}

/* Render the player's 80x48 banked Aether-Interceptor (or 1st-person cockpit). */
static void draw_player_starfighter(void){
  int aim_px = (VIEW_W >> 1) + g_player.aim_x;
  int aim_py = (VIEW_H >> 1) + g_player.aim_y;

  if (g_player.cockpit_view){
    /* 1st-person armored cockpit canopy frame and twin nose cannons */
    for (int t = -1; t <= 1; ++t){
      draw_line_px(0, 184 + t, 68, 126 + t, 0x0567);
      draw_line_px(68, 126 + t, 252, 126 + t, 0x089A);
      draw_line_px(252, 126 + t, 319, 184 + t, 0x0567);
      draw_line_px(32 + t, 191, 62 + t, 154, 0x09BC);
      draw_line_px(287 + t, 191, 257 + t, 154, 0x09BC);
    }
    if (g_player.fire_cooldown >= 3){
      draw_line_px(62, 154, aim_px - 3, aim_py, 0x0FF2);
      draw_line_px(257, 154, aim_px + 3, aim_py, 0x0FF2);
    }
    return;
  }

  /* Select one of the 5 banked 80x48 starfighter frames (-2..+2) */
  int eff_roll = g_player.roll;
  if (g_player.barrel_spin > 0){
    eff_roll = (isin(g_player.barrel_spin) * 32) >> FIX_SHIFT;
  }
  int bank_idx = 2 + (eff_roll / 10);
  if (bank_idx < 0) bank_idx = 0;
  if (bank_idx >= SHIP_FRAMES) bank_idx = SHIP_FRAMES - 1;

  int ship_x0 = (VIEW_W - SHIP_W) / 2 + (g_player.roll >> 1) + (g_player.aim_x >> 3);
  int ship_y0 = 132 + (g_player.pitch >> 2) + (g_player.aim_y >> 3);

  unsigned short* frame_img = ship_images + bank_idx * (SHIP_W * SHIP_H);
  unsigned char* frame_spans = ship_spans + bank_idx * (SHIP_W * 2);

  for (int c = 0; c < SHIP_W; ++c){
    int x = ship_x0 + c;
    if ((unsigned)x >= VIEW_W) continue;
    int first = frame_spans[c << 1];
    int last = frame_spans[(c << 1) + 1];
    if (last <= first) continue;
    int top = ship_y0 + first;
    int bot = ship_y0 + last;
    if (top < 0){
      first -= top;
      top = 0;
    }
    if (bot > VIEW_H) bot = VIEW_H;
    if (bot <= top) continue;
    draw_image_col(view_at(x, top), bot - top,
                   frame_img + c * SHIP_H + first, HALF_LEFT(top));
  }

  /* Animated twin afterburner exhaust plumes */
  int ship_cx = ship_x0 + (SHIP_W >> 1);
  int thr_y = ship_y0 + 32;
  int flame_len = g_player.boost_active ? (8 + (g_frame & 3)) : (4 + (g_frame & 1));
  for (int side = -10; side <= 10; side += 20){
    int tx = ship_cx + side;
    for (int dy = 0; dy < flame_len; ++dy){
      unsigned short fc = (dy < 3) ? 0x0FFF : ((dy < 6) ? 0x04FF : 0x028F);
      put_px(tx - 1, thr_y + dy, fc);
      put_px(tx,     thr_y + dy, 0x0FFF);
      put_px(tx + 1, thr_y + dy, fc);
    }
  }

  /* Converging twin/quad wingtip plasma beams when firing */
  if (g_player.fire_cooldown >= 3){
    unsigned short bcol = (g_player.weapon_level >= 2) ? 0x04FF : 0x0FF2;
    int l_wing_x = ship_cx - 32;
    int r_wing_x = ship_cx + 32;
    int wing_y = ship_y0 + 28;
    draw_line_px(l_wing_x, wing_y, aim_px - 2, aim_py, bcol);
    draw_line_px(l_wing_x + 1, wing_y, aim_px - 1, aim_py, 0x0FFF);
    draw_line_px(r_wing_x, wing_y, aim_px + 2, aim_py, bcol);
    draw_line_px(r_wing_x - 1, wing_y, aim_px + 1, aim_py, 0x0FFF);
  }

  /* Hexagonal deflector shield aura during barrel roll */
  if (g_player.barrel_spin > 0){
    unsigned short sc = (g_frame & 2) ? 0x0FF4 : 0x0AF2;
    int cy = ship_y0 + 24;
    draw_line_px(ship_cx - 42, cy, ship_cx - 20, cy - 20, sc);
    draw_line_px(ship_cx - 20, cy - 20, ship_cx + 20, cy - 20, sc);
    draw_line_px(ship_cx + 20, cy - 20, ship_cx + 42, cy, sc);
    draw_line_px(ship_cx + 42, cy, ship_cx + 20, cy + 18, sc);
    draw_line_px(ship_cx + 20, cy + 18, ship_cx - 20, cy + 18, sc);
    draw_line_px(ship_cx - 20, cy + 18, ship_cx - 42, cy, sc);
  }
}

/* Render the full 320x192 single-pixel 3D Voxel-Space scene and billboards. */
void render_scene(struct Camera* cam){
  int cos_a = icos(cam->angle);
  int sin_a = isin(cam->angle);
  int right_x = -sin_a;
  int right_y = cos_a;

  int cam_z_q8 = cam->z >> 8;
  int horizon_y = (VIEW_H >> 1) + 4 + cam->pitch;
  if (horizon_y < 16) horizon_y = 16;
  if (horizon_y > VIEW_H - 16) horizon_y = VIEW_H - 16;

  int sun_angle = (768 - cam->angle) & ANGLE_MASK;
  if (sun_angle > 512) sun_angle -= 1024;
  int sun_cx = (VIEW_W >> 1) + sun_angle;
  int sun_cy = 34 + (cam->pitch >> 1);
  unsigned short* sky_grad = &sky_gradients[(cam->biome & 7) << 5];
  int wave_scroll = (int)(g_frame << 1);

  /* 1. March 320 single-pixel columns across 40 TLB-friendly column-groups */
  for (int x = 0; x < VIEW_W; ++x){
    int fov_off = x - (VIEW_W >> 1); /* -160..+159 */
    int ray_dx = cos_a + ((right_x * fov_off) >> 7);
    int ray_dy = sin_a + ((right_y * fov_off) >> 7);
    int roll_off = (fov_off * cam->roll) >> 7;

    int y_top = VIEW_H;
    unsigned char* col_occl = &occl_top[x << 4];

    int rx = cam->x + ray_dx + (ray_dx >> 2);
    int ry = cam->y + ray_dy + (ray_dy >> 2);
    int step0_x = ray_dx >> 1;                   /* 0.500 cells */
    int step0_y = ray_dy >> 1;
    int step1_x = ray_dx - (ray_dx >> 3);        /* 0.875 cells */
    int step1_y = ray_dy - (ray_dy >> 3);
    int step2_x = ray_dx + (ray_dx >> 2) + (ray_dx >> 3); /* 1.375 cells */
    int step2_y = ray_dy + (ray_dy >> 2) + (ray_dy >> 3);

    for (int s = 0; s < VOXEL_STEPS; ++s){
      int wx0 = (rx >> FIX_SHIFT) & MAP_MASK;
      int wy0 = (ry >> FIX_SHIFT) & MAP_MASK;
      unsigned short cell00 = g_terrain[(wy0 << MAP_SHIFT) | wx0];
      int h00 = cell00 & 0x3F;
      int light = (cell00 >> 14) & 3;

      /* Smooth bilinear 8.8 height interpolation on near and mid steps */
      int h_q8 = h00 << 8;
      if (s < 30){
        int wx1 = (wx0 + 1) & MAP_MASK;
        int wy1 = (wy0 + 1) & MAP_MASK;
        int fx = (rx >> 12) & 15;
        int fy = (ry >> 12) & 15;
        int h10 = g_terrain[(wy0 << MAP_SHIFT) | wx1] & 0x3F;
        int h01 = g_terrain[(wy1 << MAP_SHIFT) | wx0] & 0x3F;
        int h11 = g_terrain[(wy1 << MAP_SHIFT) | wx1] & 0x3F;
        int h_top = h00 * (16 - fx) + h10 * fx;
        int h_bot = h01 * (16 - fx) + h11 * fx;
        h_q8 = h_top * (16 - fy) + h_bot * fy; /* 16*16 = 256 -> 8.8 fixed point! */
      }

      int scale = step_scale[s];
      int y = horizon_y + (((cam_z_q8 - h_q8) * scale) >> 11) + roll_off;
      if (y < 0) y = 0;
      if (y < y_top){
        int tex_id = select_terrain_tex(cam->biome, h00, light);
        int uv_shift = (h00 <= WATER_LEVEL) ? 14 : 13;
        int tex_u = ((rx >> uv_shift) + (h00 <= WATER_LEVEL ? (wave_scroll >> 2) : 0)) & 63;
        unsigned char* texcol = terrain_textures + (tex_id << 12) + (tex_u << 6);

        int shade_lvl = LIGHT_FULL - (s >> 2) + (light == 2 ? 2 : (light == 0 ? -2 : 0));
        if (light == 3) shade_lvl = LIGHT_LEVELS - 1;
        if (shade_lvl < 1) shade_lvl = 1;
        if (shade_lvl >= LIGHT_LEVELS) shade_lvl = LIGHT_LEVELS - 1;

        int tex_v = ((ry >> uv_shift) + (h00 <= WATER_LEVEL ? (wave_scroll >> 1) : 0)) & 63;
        int tex_pos = tex_v << 16;
        int tex_step = (h00 <= WATER_LEVEL) ? 0x02000 : ((s < 16) ? 0x06000 : 0x0C000);
        draw_voxel_span(view_at(x, y), y_top - y, texcol, tex_pos, tex_step,
                        shade_tables + (shade_lvl << 8), HALF_LEFT(y));
        y_top = y;
      }
      if ((s & 3) == 3){
        col_occl[s >> 2] = (unsigned char)y_top;
      }
      if (s < 18){
        rx += step0_x;
        ry += step0_y;
      } else if (s < 34){
        rx += step1_x;
        ry += step1_y;
      } else {
        rx += step2_x;
        ry += step2_y;
      }
    }

    /* Fill remaining sky rows [0 .. y_top - 1] with banking sky gradient & sun */
    int sdx = (x - sun_cx) >> 1;
    bool near_sun_x = (sdx >= -7 && sdx <= 7);
    int sdx2 = near_sun_x ? (sdx * sdx) : 64;
    for (int y = 0; y < y_top; ++y){
      int sky_band = (y - roll_off - (cam->pitch >> 1)) >> 2;
      if (sky_band < 0) sky_band = 0;
      if (sky_band > 31) sky_band = 31;
      unsigned short scol = sky_grad[sky_band];

      if (near_sun_x){
        int sdy = (y - sun_cy) >> 1;
        if (sdy >= -7 && sdy <= 7){
          int sd2 = sdx2 + sdy * sdy;
          if (sd2 <= 42){
            scol = (sd2 <= 14) ? 0x0FFF : 0x06DF;
          }
        }
      } else if (sky_band < 12 && ((((x << 5) ^ (y << 3) ^ x ^ y) & 255) == 0)){
        scol = 0x0EEE;
      }
      *view_at(x, y) = scol;
    }
  }

  /* 2. In Terrain Forge mode, draw the 3D sculpting brush ring on the terrain */
  if (g_mode == MODE_FORGE){
    draw_forge_cursor_ring(cam);
  }

  /* 3. Depth-sort active 3D entities back-to-front so nearer objects overlap farther ones */
  int order[MAX_ENTITIES];
  int dists[MAX_ENTITIES];
  int vis_count = 0;
  int dir_x = icos(cam->angle);
  int dir_y = isin(cam->angle);
  for (int i = 0; i < MAX_ENTITIES; ++i){
    if (!g_entities[i].active) continue;
    int dx = g_entities[i].x - cam->x;
    int dy = g_entities[i].y - cam->y;
    int half_map = 64 << FIX_SHIFT;
    int full_map = 128 << FIX_SHIFT;
    while (dx > half_map) dx -= full_map;
    while (dx < -half_map) dx += full_map;
    while (dy > half_map) dy -= full_map;
    while (dy < -half_map) dy += full_map;
    int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    if (fwd < (2 << FIX_SHIFT) || fwd > (50 << FIX_SHIFT)) continue;
    order[vis_count] = i;
    dists[vis_count] = fwd;
    vis_count += 1;
  }
  /* Insertion sort from farthest to nearest */
  for (int i = 1; i < vis_count; ++i){
    int k_ord = order[i];
    int k_dst = dists[i];
    int j = i - 1;
    while (j >= 0 && dists[j] < k_dst){
      order[j + 1] = order[j];
      dists[j + 1] = dists[j];
      j -= 1;
    }
    order[j + 1] = k_ord;
    dists[j + 1] = k_dst;
  }
  for (int i = 0; i < vis_count; ++i){
    draw_entity_3d(cam, &g_entities[order[i]]);
  }

  /* 4. Render 3D projectiles as glowing 64x64 energy orb billboards */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    int sx = 0, sy = 0, sstep = 0, sscale = 0;
    if (project_point3d(cam, s->x, s->y, s->z, &sx, &sy, &sstep, &sscale)){
      int spr = SPR_ORB_PLAYER;
      int sz = (sscale * 3) >> 2;
      if (s->kind == SHOT_PLAYER_SEISMIC){
        spr = SPR_ORB_SEISMIC;
        sz = (sscale * 5) >> 2;
      } else if (s->kind >= SHOT_ENEMY_LASER){
        spr = SPR_ORB_ENEMY;
        sz = sscale;
      }
      draw_billboard_sprite(sx, sy, sz < 4 ? 4 : sz, spr, LIGHT_LEVELS - 1, sstep >> 2);
    }
  }

  /* 5. Render 3D explosion sparks and debris particles */
  for (int i = 0; i < MAX_PARTICLES; ++i){
    struct Particle* p = &g_particles[i];
    if (!p->active) continue;
    int px = 0, py = 0, pstep = 0, pscale = 0;
    if (project_point3d(cam, p->x, p->y, p->z, &px, &py, &pstep, &pscale)){
      int b = pstep >> 2;
      if ((unsigned)px < VIEW_W - 1 && (unsigned)py < VIEW_H - 1 &&
          py < (int)occl_top[(px << 4) | b]){
        put_px(px, py, p->color);
        put_px(px + 1, py, p->color);
        put_px(px, py + 1, p->color);
      }
    }
  }

  /* 6. Render the player's 80x48 banked Starfighter / Cockpit in foreground */
  if (g_mode != MODE_FORGE || g_state == STATE_TITLE){
    draw_player_starfighter();
  }
}

/* Compute a deterministic 32-bit FNV-1a checksum of the rendered 3D backbuffer. */
unsigned render_view_checksum(void){
  unsigned hash = 2166136261u;
  for (int i = 0; i < VIEW_WORDS; ++i){
    hash ^= g_view_words[i];
    hash *= 16777619u;
  }
  return hash;
}
