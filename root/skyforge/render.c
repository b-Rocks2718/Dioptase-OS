#include "skyforge.h"

/*
 * 3D Voxel-Space terrain and water renderer, banking sky and sun synthesizer,
 * per-column mountain-ridge occlusion buffer, 3D world object rasterizer, and
 * 3D polygonal Aether-Interceptor starfighter renderer.
 */

/* Per-frame projected screen row (0..VIEW_H) for each depth step and height. */
static unsigned char frame_y_lut[3072]; /* VOXEL_STEPS (48) * 64 */

/* Per-frame 32-bit pixel-pair palette across 4 distance fog bands (4 * 256). */
static unsigned frame_pal_words[1024];

/* Per-column mountain silhouette top row at 16 depth buckets (160 * 16). */
static unsigned char occl_top[2560];

/* Precomputed ray distance (in 8.8 cells) and height projection scale for
 * each of the 48 voxel raymarching steps. */
static int step_dist_q8[48];
static int step_scale[48];

/* Initialize raymarching distance and perspective scale tables. */
void render_init(void){
  int dist_q8 = 384; /* Start 1.5 cells in front of camera */
  for (int s = 0; s < VOXEL_STEPS; ++s){
    step_dist_q8[s] = dist_q8;
    /* Perspective scale: 170 * 256 / dist_q8 */
    step_scale[s] = (170 * 256) / dist_q8;
    if (s < 16){
      dist_q8 += 160; /* 0.625 cells per step near camera */
    } else if (s < 32){
      dist_q8 += 256; /* 1.0 cell per step mid-range */
    } else {
      dist_q8 += 384; /* 1.5 cells per step far range */
    }
  }
}

/* Plot one 32-bit pixel pair at double-column `cx` (0..159) and row `y` (0..191). */
static void put_pair(int cx, int y, unsigned pair){
  if ((unsigned)cx >= VIEW_COLS || (unsigned)y >= VIEW_H) return;
  g_view_words[VIEW_WORD_INDEX(cx, y)] = pair;
}

/* Plot a depth-tested 32-bit pixel pair that respects mountain ridge occlusion. */
static void put_pair_occl(int cx, int y, int bucket, unsigned pair){
  if ((unsigned)cx >= VIEW_COLS || (unsigned)y >= VIEW_H) return;
  if (bucket < 0) bucket = 0;
  if (bucket >= OCCL_BUCKETS) bucket = OCCL_BUCKETS - 1;
  if (y < (int)occl_top[(cx << 4) | bucket]){
    g_view_words[VIEW_WORD_INDEX(cx, y)] = pair;
  }
}

/* Draw a filled depth-tested rectangle in double-column coordinates. */
static void fill_rect_occl(int cx0, int y0, int cx1, int y1, int bucket, unsigned pair){
  if (cx0 < 0) cx0 = 0;
  if (y0 < 0) y0 = 0;
  if (cx1 >= VIEW_COLS) cx1 = VIEW_COLS - 1;
  if (y1 >= VIEW_H) y1 = VIEW_H - 1;
  if (cx0 > cx1 || y0 > y1) return;
  if (bucket < 0) bucket = 0;
  if (bucket >= OCCL_BUCKETS) bucket = OCCL_BUCKETS - 1;
  for (int cx = cx0; cx <= cx1; ++cx){
    int max_y = (int)occl_top[(cx << 4) | bucket];
    int end_y = y1 < max_y - 1 ? y1 : max_y - 1;
    int base = (((cx >> 3) << 10) + ((cx >> 3) << 9)) + (cx & 7);
    for (int y = y0; y <= end_y; ++y){
      g_view_words[base + (y << 3)] = pair;
    }
  }
}

/* Draw a 2D line in double-column view coordinates (cx: 0..159, y: 0..191). */
static void draw_line_pair(int x0, int y0, int x1, int y1, unsigned pair){
  int dx = x1 - x0;
  int dy = y1 - y0;
  int sx = dx >= 0 ? 1 : -1;
  int sy = dy >= 0 ? 1 : -1;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  int err = (dx > dy ? dx : -dy) / 2;
  for (int steps = 0; steps < 256; ++steps){
    put_pair(x0, y0, pair);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err;
    if (e2 > -dx){ err -= dy; x0 += sx; }
    if (e2 <  dy){ err += dx; y0 += sy; }
  }
}

/* Fill a flat-bottom or general triangle in double-column view coordinates. */
static void fill_tri_pair(int x0, int y0, int x1, int y1, int x2, int y2, unsigned pair){
  /* Sort vertices by Y: y0 <= y1 <= y2 */
  if (y0 > y1){ int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  if (y1 > y2){ int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
  if (y0 > y1){ int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  if (y0 == y2) return;

  int total_h = y2 - y0;
  for (int y = y0; y <= y2; ++y){
    if (y < 0 || y >= VIEW_H) continue;
    bool second_half = (y > y1) || (y1 == y0);
    int seg_h = second_half ? (y2 - y1) : (y1 - y0);
    if (seg_h == 0) continue;
    int xa = x0 + ((x2 - x0) * (y - y0)) / total_h;
    int xb = second_half
             ? (x1 + ((x2 - x1) * (y - y1)) / seg_h)
             : (x0 + ((x1 - x0) * (y - y0)) / seg_h);
    if (xa > xb){ int t = xa; xa = xb; xb = t; }
    if (xa < 0) xa = 0;
    if (xb >= VIEW_COLS) xb = VIEW_COLS - 1;
    for (int cx = xa; cx <= xb; ++cx){
      g_view_words[VIEW_WORD_INDEX(cx, y)] = pair;
    }
  }
}

/* Prepare the per-frame height projection LUT and distance-fogged biome palette. */
static void prepare_frame_tables(struct Camera* cam){
  int cam_h = cam->z >> FIX_SHIFT;
  int horizon_y = (VIEW_H >> 1) + 4 + cam->pitch;
  if (horizon_y < 16) horizon_y = 16;
  if (horizon_y > VIEW_H - 16) horizon_y = VIEW_H - 16;

  for (int s = 0; s < VOXEL_STEPS; ++s){
    int scale = step_scale[s];
    unsigned char* row_lut = &frame_y_lut[s << 6];
    for (int h = 0; h < 64; ++h){
      int y = horizon_y + (((cam_h - h) * scale) >> 3);
      if (y < 0) y = 0;
      if (y > VIEW_H) y = VIEW_H;
      row_lut[h] = (unsigned char)y;
    }
  }

  int b = cam->biome;
  if (b < 0 || b >= BIOME_COUNT) b = 0;
  unsigned short fog_col = sky_gradients[(b << 5) + 30];
  unsigned short* base_pal = &biome_palettes[b << 8];
  int wave_phase = (g_frame >> 2) & 3;

  for (int band = 0; band < 4; ++band){
    int fog_level = band * 2; /* 0, 2, 4, 6 */
    unsigned* dst = &frame_pal_words[band << 8];
    for (int idx = 0; idx < 256; ++idx){
      int h = idx & 0x3F;
      int light = (idx >> 6) & 3;
      unsigned short col = base_pal[idx];
      /* Animate water/lava shimmer in real time */
      if (h <= WATER_LEVEL && ((h + light + wave_phase) & 3) == 0){
        col = base_pal[(2 << 6) | h];
      }
      unsigned short shaded = (light == 3 && band < 3)
                              ? col
                              : shade_color12(col, fog_col, fog_level);
      dst[idx] = pack_pair(shaded);
    }
  }
}

/* Project a 3D world point (wx, wy, wz) into view coordinates (out_cx, out_y, out_step).
 * Returns true if the point lies in front of the near clip plane. */
static bool project_point3d(struct Camera* cam, int wx, int wy, int wz,
                            int* out_cx, int* out_y, int* out_step){
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
  if (fwd < (2 << FIX_SHIFT) || fwd > (54 << FIX_SHIFT)) return false;
  int side = fixmul(dx, right_x) + fixmul(dy, right_y);

  int dist_q8 = fwd >> 8;
  if (dist_q8 < 256) dist_q8 = 256;

  int cx = (VIEW_COLS >> 1) + (((side >> 8) * 96) / dist_q8);
  int horizon_y = (VIEW_H >> 1) + 4 + cam->pitch;
  int roll_off = ((cx - (VIEW_COLS >> 1)) * cam->roll) >> 6;
  int scale = (170 * 256) / dist_q8;
  int dh = (cam->z - wz) >> FIX_SHIFT;
  int y = horizon_y + ((dh * scale) >> 3) + roll_off;

  /* Map distance to approximate ray step (0..47) */
  int step = (dist_q8 - 384) / 240;
  if (step < 0) step = 0;
  if (step >= VOXEL_STEPS) step = VOXEL_STEPS - 1;

  *out_cx = cx;
  *out_y = y;
  *out_step = step;
  return true;
}

/* Draw the 3D Terrain Forge brush ring projected onto the terrain heightmap. */
static void draw_forge_cursor_ring(struct Camera* cam){
  int cx0 = g_player.forge_cursor_x;
  int cy0 = g_player.forge_cursor_y;
  int rad = g_player.forge_radius;
  unsigned ring_col = pack_pair((g_frame & 4) ? 0x0FF2 : 0x02FF);

  for (int a = 0; a < ANGLE_COUNT; a += 64){
    int wx = ((cx0 << FIX_SHIFT) + FIX_HALF) + (icos(a) * rad);
    int wy = ((cy0 << FIX_SHIFT) + FIX_HALF) + (isin(a) * rad);
    int wz = terrain_height_at(wx, wy) + (1 << FIX_SHIFT);
    int scx = 0, sy = 0, sstep = 0;
    if (project_point3d(cam, wx, wy, wz, &scx, &sy, &sstep)){
      fill_rect_occl(scx - 1, sy - 2, scx + 1, sy + 1, sstep >> 2, ring_col);
    }
  }
}

/* Draw one 3D entity (ring, gate, arch, spire, beacon, enemy, or boss). */
static void draw_entity_3d(struct Camera* cam, struct Entity* e){
  int cx = 0, cy = 0, step = 0;
  if (!project_point3d(cam, e->x, e->y, e->z, &cx, &cy, &step)) return;
  if (cx < -24 || cx >= VIEW_COLS + 24 || cy < -32 || cy >= VIEW_H + 32) return;

  int bucket = step >> 2;
  int scale = step_scale[step];
  int rad_x = (scale * 3) >> 4;
  int rad_y = (scale * 6) >> 4;
  if (rad_x < 2) rad_x = 2;
  if (rad_y < 4) rad_y = 4;

  if (e->kind == ENT_RING || e->kind == ENT_GATE){
    /* 3D Octagonal Power Ring / Grand Prix Gate */
    bool is_next_gate = (e->kind == ENT_GATE && e->phase == g_player.next_gate);
    unsigned short c_outer = (e->kind == ENT_RING)
                             ? ((g_frame & 4) ? 0x02DF : 0x04FF)
                             : (is_next_gate ? ((g_frame & 2) ? 0x04F2 : 0x0FF4) : 0x0862);
    unsigned short c_inner = 0x0FFF;
    unsigned p_outer = pack_pair(c_outer);
    unsigned p_inner = pack_pair(c_inner);
    int rx = rad_x + 2;
    int ry = rad_y + 4;
    /* Top and bottom horizontal bars */
    fill_rect_occl(cx - (rx >> 1), cy - ry, cx + (rx >> 1), cy - ry + 2, bucket, p_outer);
    fill_rect_occl(cx - (rx >> 1), cy + ry - 2, cx + (rx >> 1), cy + ry, bucket, p_outer);
    /* Left and right vertical pillars */
    fill_rect_occl(cx - rx, cy - (ry >> 1), cx - rx + 1, cy + (ry >> 1), bucket, p_outer);
    fill_rect_occl(cx + rx - 1, cy - (ry >> 1), cx + rx, cy + (ry >> 1), bucket, p_outer);
    /* Diagonal corner struts */
    fill_rect_occl(cx - rx + 1, cy - ry + 2, cx - (rx >> 1), cy - (ry >> 1), bucket, p_inner);
    fill_rect_occl(cx + (rx >> 1), cy - ry + 2, cx + rx - 1, cy - (ry >> 1), bucket, p_inner);
    fill_rect_occl(cx - rx + 1, cy + (ry >> 1), cx - (rx >> 1), cy + ry - 2, bucket, p_inner);
    fill_rect_occl(cx + (rx >> 1), cy + (ry >> 1), cx + rx - 1, cy + ry - 2, bucket, p_inner);
  } else if (e->kind == ENT_ARCH){
    /* Massive 3D Canyon Stone Arch spanning the river */
    int rx = (rad_x * 5) >> 1;
    int ry = (rad_y * 5) >> 1;
    unsigned p_stone = pack_pair(0x0678);
    unsigned p_rune = pack_pair(0x0AF4);
    fill_rect_occl(cx - rx, cy - ry, cx - rx + (rad_x >> 1) + 1, cy + ry, bucket, p_stone);
    fill_rect_occl(cx + rx - (rad_x >> 1) - 1, cy - ry, cx + rx, cy + ry, bucket, p_stone);
    fill_rect_occl(cx - rx, cy - ry, cx + rx, cy - ry + (rad_y >> 1) + 1, bucket, p_stone);
    fill_rect_occl(cx - (rx >> 1), cy - ry + 1, cx + (rx >> 1), cy - ry + 2, bucket, p_rune);
  } else if (e->kind == ENT_SPIRE || e->kind == ENT_BEACON){
    /* Glowing Crystal Spire or Rescue Beacon */
    unsigned p_col = pack_pair(e->kind == ENT_BEACON ? 0x0FF2 : 0x09F3);
    unsigned p_core = pack_pair(0x0FFF);
    fill_rect_occl(cx - (rad_x >> 1), cy - rad_y, cx + (rad_x >> 1), cy + rad_y, bucket, p_col);
    fill_rect_occl(cx, cy - rad_y - 4, cx, cy + rad_y, bucket, p_core);
  } else if (e->kind == ENT_BOSS){
    /* Colossal Leviathan Dreadnought Boss with 4 orbiting shield pylons */
    int bx = rad_x * 3;
    int by = rad_y * 2;
    unsigned p_hull = pack_pair(e->flash ? 0x0FFF : 0x0437);
    unsigned p_trim = pack_pair(0x035F);
    unsigned p_core = pack_pair((g_frame & 2) ? 0x02FF : 0x044F);
    /* Main armored dreadnought wings and central citadel */
    fill_rect_occl(cx - bx, cy - (by >> 2), cx + bx, cy + (by >> 2), bucket, p_hull);
    fill_rect_occl(cx - (bx >> 1), cy - by, cx + (bx >> 1), cy + by, bucket, p_hull);
    fill_rect_occl(cx - bx, cy, cx + bx, cy + 1, bucket, p_trim);
    fill_rect_occl(cx - (bx >> 2), cy - (by >> 1), cx + (bx >> 2), cy + (by >> 1), bucket, p_core);
    /* 4 Shield Generator Pylons */
    for (int g = 0; g < 4; ++g){
      if (!(e->shield_gen & (1 << g))) continue;
      int ang = ((int)g_frame << 3) + (g << 8);
      int px = cx + ((icos(ang) * (bx + 4)) >> FIX_SHIFT);
      int py = cy + ((isin(ang) * (by + 2)) >> FIX_SHIFT);
      fill_rect_occl(px - 2, py - 4, px + 2, py + 4, bucket, pack_pair(0x0FF3));
    }
    /* Boss overhead health bar */
    int bar_w = 24;
    int fill_w = (e->hp * bar_w) / e->max_hp;
    fill_rect_occl(cx - 12, cy - by - 6, cx + 12, cy - by - 4, bucket, pack_pair(0x0222));
    if (fill_w > 0){
      fill_rect_occl(cx - 12, cy - by - 6, cx - 12 + fill_w, cy - by - 4, bucket, pack_pair(0x024F));
    }
  } else {
    /* Enemy Fighter / Interceptor / Stealth / Turret / Rival */
    unsigned short hull_c = 0x034C;
    unsigned short wing_c = 0x0228;
    if (e->kind == ENT_INTERCEPTOR){ hull_c = 0x026F; wing_c = 0x013A; }
    else if (e->kind == ENT_STEALTH){ hull_c = 0x0B38; wing_c = 0x0615; }
    else if (e->kind == ENT_TURRET){ hull_c = 0x0677; wing_c = 0x039B; }
    else if (e->kind == ENT_RIVAL){ hull_c = 0x03DF; wing_c = 0x018C; }
    if (e->flash > 0){ hull_c = 0x0FFF; wing_c = 0x0FFF; }

    /* Swept wings, central fuselage, and glowing engine core */
    fill_rect_occl(cx - rad_x - 2, cy - 1, cx + rad_x + 2, cy + 2, bucket, pack_pair(wing_c));
    fill_rect_occl(cx - (rad_x >> 1), cy - (rad_y >> 1), cx + (rad_x >> 1), cy + (rad_y >> 1), bucket, pack_pair(hull_c));
    fill_rect_occl(cx, cy - 1, cx, cy + 1, bucket, pack_pair(0x04FF));
  }
}

/* Rotate a 2D point (px, py) around the origin by 10-bit angle `ang`. */
static void rot2d(int px, int py, int ang, int* ox, int* oy){
  int c = icos(ang);
  int s = isin(ang);
  *ox = (px * c - py * s) >> FIX_SHIFT;
  *oy = (px * s + py * c) >> FIX_SHIFT;
}

/* Render the player's 3D polygonal Aether-Interceptor (or 1st-person cockpit). */
static void draw_player_starfighter(void){
  if (g_player.cockpit_view){
    /* 1st-person armored cockpit canopy struts and twin nose cannons */
    unsigned strut = pack_pair(0x0456);
    unsigned barrel = pack_pair(0x089A);
    draw_line_pair(0, 180, 36, 130, strut);
    draw_line_pair(36, 130, 124, 130, strut);
    draw_line_pair(124, 130, 159, 180, strut);
    draw_line_pair(18, 191, 32, 160, barrel);
    draw_line_pair(141, 191, 127, 160, barrel);
    return;
  }

  /* 3rd-person Chase Cam: 3D Polygonal Arwing Starfighter */
  int ship_cx = (VIEW_COLS >> 1) + (g_player.roll >> 2);
  int ship_cy = 154 + (g_player.pitch >> 2);
  int roll_ang = ((g_player.roll * 6) + g_player.barrel_spin) & ANGLE_MASK;

  int nx = 0, ny = 0;     /* Nose apex */
  int lwx = 0, lwy = 0;   /* Left outer wingtip */
  int rwx = 0, rwy = 0;   /* Right outer wingtip */
  int lix = 0, liy = 0;   /* Left inner wing root */
  int rix = 0, riy = 0;   /* Right inner wing root */
  int tx = 0, ty = 0;     /* Center tail */
  int lfx = 0, lfy = 0;   /* Left vertical stabilizer fin */
  int rfx = 0, rfy = 0;   /* Right vertical stabilizer fin */

  int nose_lift = -8 - (g_player.pitch >> 2);
  rot2d(0, nose_lift, roll_ang, &nx, &ny);
  rot2d(-22, 6, roll_ang, &lwx, &lwy);
  rot2d(22, 6, roll_ang, &rwx, &rwy);
  rot2d(-6, 2, roll_ang, &lix, &liy);
  rot2d(6, 2, roll_ang, &rix, &riy);
  rot2d(0, 8, roll_ang, &tx, &ty);
  rot2d(-8, -6, roll_ang, &lfx, &lfy);
  rot2d(8, -6, roll_ang, &rfx, &rfy);

  unsigned short c_left_wing  = (g_player.hurt_flash > 0) ? 0x044F : 0x0A74;
  unsigned short c_right_wing = (g_player.hurt_flash > 0) ? 0x044F : 0x0C96;
  unsigned short c_fuselage   = (g_player.hurt_flash > 0) ? 0x0FFF : 0x0EDF;
  unsigned short c_fin        = 0x09C2;
  unsigned short c_canopy     = 0x0FF3;

  /* Swept left and right delta wings */
  fill_tri_pair(ship_cx + nx, ship_cy + ny,
                ship_cx + lwx, ship_cy + lwy,
                ship_cx + lix, ship_cy + liy,
                pack_pair(c_left_wing));
  fill_tri_pair(ship_cx + nx, ship_cy + ny,
                ship_cx + rix, ship_cy + riy,
                ship_cx + rwx, ship_cy + rwy,
                pack_pair(c_right_wing));

  /* Central armored fuselage */
  fill_tri_pair(ship_cx + nx, ship_cy + ny,
                ship_cx + lix, ship_cy + liy,
                ship_cx + tx, ship_cy + ty,
                pack_pair(c_fuselage));
  fill_tri_pair(ship_cx + nx, ship_cy + ny,
                ship_cx + tx, ship_cy + ty,
                ship_cx + rix, ship_cy + riy,
                pack_pair(c_right_wing));

  /* Twin emerald dioptase dorsal stabilizer fins */
  fill_tri_pair(ship_cx + lix, ship_cy + liy,
                ship_cx + lfx, ship_cy + lfy,
                ship_cx + tx, ship_cy + ty,
                pack_pair(c_fin));
  fill_tri_pair(ship_cx + rix, ship_cy + riy,
                ship_cx + rfx, ship_cy + rfy,
                ship_cx + tx, ship_cy + ty,
                pack_pair(c_fin));

  /* Cockpit canopy & glowing twin plasma thruster cores */
  put_pair(ship_cx + (nx >> 1), ship_cy + (ny >> 1), pack_pair(c_canopy));
  unsigned short thrust_outer = g_player.boost_active ? 0x04FF : 0x02AF;
  unsigned short thrust_core  = 0x0FFF;
  int flame_r = g_player.boost_active ? 4 : 2;
  for (int dy = -flame_r; dy <= flame_r; ++dy){
    put_pair(ship_cx + (lix >> 1) - 1, ship_cy + ty + dy, pack_pair(thrust_outer));
    put_pair(ship_cx + (rix >> 1) + 1, ship_cy + ty + dy, pack_pair(thrust_outer));
  }
  put_pair(ship_cx + (lix >> 1) - 1, ship_cy + ty, pack_pair(thrust_core));
  put_pair(ship_cx + (rix >> 1) + 1, ship_cy + ty, pack_pair(thrust_core));

  /* Wingtip converging plasma beams when cannons fire */
  if (g_player.fire_cooldown >= 4){
    int aim_cx = (VIEW_COLS >> 1) + (g_player.aim_x >> 1);
    int aim_cy = (VIEW_H >> 1) + (g_player.aim_y >> 1);
    unsigned beam_col = pack_pair((g_player.weapon_level >= 2) ? 0x04FF : 0x0FF2);
    draw_line_pair(ship_cx + lwx, ship_cy + lwy, aim_cx - 2, aim_cy, beam_col);
    draw_line_pair(ship_cx + rwx, ship_cy + rwy, aim_cx + 2, aim_cy, beam_col);
  }

  /* Barrel-roll deflector shield ring */
  if (g_player.barrel_spin > 0){
    unsigned shield_c = pack_pair((g_frame & 2) ? 0x0FF4 : 0x0AF2);
    draw_line_pair(ship_cx - 24, ship_cy, ship_cx, ship_cy - 18, shield_c);
    draw_line_pair(ship_cx, ship_cy - 18, ship_cx + 24, ship_cy, shield_c);
    draw_line_pair(ship_cx + 24, ship_cy, ship_cx, ship_cy + 16, shield_c);
    draw_line_pair(ship_cx, ship_cy + 16, ship_cx - 24, ship_cy, shield_c);
  }
}

/* Render the full 3D Voxel-Space landscape, sky, objects, and player craft. */
void render_scene(struct Camera* cam){
  prepare_frame_tables(cam);

  int cos_a = icos(cam->angle);
  int sin_a = isin(cam->angle);
  int right_x = -sin_a;
  int right_y = cos_a;

  int sun_angle = (768 - cam->angle) & ANGLE_MASK;
  if (sun_angle > 512) sun_angle -= 1024;
  int sun_cx = (VIEW_COLS >> 1) + (sun_angle >> 1);
  int sun_cy = 34 + (cam->pitch >> 1);
  unsigned short* sky_grad = &sky_gradients[(cam->biome & 7) << 5];

  /* 1. March 160 double-columns grouped into 20 TLB-sized column-groups */
  for (int g = 0; g < VIEW_GROUPS; ++g){
    unsigned* group_base = &g_view_words[(g << 10) + (g << 9)];
    for (int c = 0; c < GROUP_COLS; ++c){
      int cx = (g << 3) | c;
      int fov_off = cx - (VIEW_COLS >> 1); /* -80..+79 */
      /* Ray direction for column cx: forward + fov_off/96 * right */
      int ray_dx = cos_a + ((right_x * fov_off) >> 6);
      int ray_dy = sin_a + ((right_y * fov_off) >> 6);

      int roll_off = (fov_off * cam->roll) >> 6;
      int y_top = VIEW_H;
      unsigned* dst = group_base + ((VIEW_H - 1) << 3) + c;
      unsigned char* col_occl = &occl_top[cx << 4];

      /* Start 1.5 cells ahead and step incrementally with shifts/adds only */
      int rx = cam->x + ray_dx + (ray_dx >> 1);
      int ry = cam->y + ray_dy + (ray_dy >> 1);
      int step0_x = (ray_dx >> 1) + (ray_dx >> 3); /* 0.625 cells */
      int step0_y = (ray_dy >> 1) + (ray_dy >> 3);
      int step1_x = ray_dx;                        /* 1.0 cell */
      int step1_y = ray_dy;
      int step2_x = ray_dx + (ray_dx >> 1);        /* 1.5 cells */
      int step2_y = ray_dy + (ray_dy >> 1);

      for (int s = 0; s < VOXEL_STEPS; ++s){
        int wx = (rx >> FIX_SHIFT) & MAP_MASK;
        int wy = (ry >> FIX_SHIFT) & MAP_MASK;
        int idx = (wy << MAP_SHIFT) | wx;
        unsigned short cell = g_terrain[idx];
        int h = cell & 0x3F;
        int col_idx = cell >> 8;

        /* Sub-cell bilinear height smoothing and micro-texture detail */
        if (s < 20){
          if (ry & 0x8000){
            int wy2 = (wy + 1) & MAP_MASK;
            int h2 = g_terrain[(wy2 << MAP_SHIFT) | wx] & 0x3F;
            h = (h + h2) >> 1;
          }
          if ((((rx >> 14) ^ (ry >> 14)) & 1) && (col_idx & 0x3F) > WATER_LEVEL + 1){
            col_idx = (col_idx & 0xC0) | ((col_idx & 0x3F) - 1);
          }
        }

        int y = (int)frame_y_lut[(s << 6) | h] + roll_off;
        if (y < 0) y = 0;
        if (y < y_top){
          unsigned pair = frame_pal_words[((s >> 4) << 8) | col_idx];
          int count = y_top - y;
          y_top = y;
          do {
            *dst = pair;
            dst -= 8;
          } while (--count > 0);
        }
        if ((s & 3) == 3){
          col_occl[s >> 2] = (unsigned char)y_top;
        }
        if (s < 16){
          rx += step0_x;
          ry += step0_y;
        } else if (s < 32){
          rx += step1_x;
          ry += step1_y;
        } else {
          rx += step2_x;
          ry += step2_y;
        }
      }

      /* Fill remaining sky rows [0 .. y_top - 1] with banking sky gradient & sun */
      int sdx = cx - sun_cx;
      bool near_sun_x = (sdx >= -6 && sdx <= 6);
      int sdx2 = near_sun_x ? (sdx * sdx) : 64;
      for (int y = y_top - 1; y >= 0; --y){
        int sky_band = (y - roll_off - (cam->pitch >> 1)) >> 2;
        if (sky_band < 0) sky_band = 0;
        if (sky_band > 31) sky_band = 31;
        unsigned short scol = sky_grad[sky_band];

        if (near_sun_x){
          int sdy = (y - sun_cy) >> 1;
          if (sdy >= -6 && sdy <= 6){
            int sd2 = sdx2 + sdy * sdy;
            if (sd2 <= 36){
              scol = (sd2 <= 12) ? 0x0FFF : 0x06DF;
            }
          }
        } else if (sky_band < 12 && ((((cx << 5) ^ (y << 3) ^ cx ^ y) & 127) == 0)){
          scol = 0x0EEE; /* High-altitude stars */
        }
        *dst = pack_pair(scol);
        dst -= 8;
      }
    }
  }

  /* 2. In Terrain Forge mode, project the 3D sculpting brush onto the terrain */
  if (g_mode == MODE_FORGE){
    draw_forge_cursor_ring(cam);
  }

  /* 3. Render 3D world entities (arches, rings, gates, beacons, enemies, boss) */
  for (int i = 0; i < MAX_ENTITIES; ++i){
    if (g_entities[i].active){
      draw_entity_3d(cam, &g_entities[i]);
    }
  }

  /* 4. Render 3D projectiles (plasma bolts, seismic torpedoes, enemy lasers) */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    int scx = 0, sy = 0, sstep = 0;
    if (project_point3d(cam, s->x, s->y, s->z, &scx, &sy, &sstep)){
      int r = (s->kind == SHOT_PLAYER_SEISMIC || s->kind == SHOT_BOSS_ORB) ? 2 : 1;
      fill_rect_occl(scx - r, sy - (r << 1), scx + r, sy + (r << 1),
                     sstep >> 2, pack_pair((unsigned short)s->color));
    }
  }

  /* 5. Render 3D ballistic explosion sparks and debris particles */
  for (int i = 0; i < MAX_PARTICLES; ++i){
    struct Particle* p = &g_particles[i];
    if (!p->active) continue;
    int pcx = 0, py = 0, pstep = 0;
    if (project_point3d(cam, p->x, p->y, p->z, &pcx, &py, &pstep)){
      put_pair_occl(pcx, py, pstep >> 2, pack_pair(p->color));
      put_pair_occl(pcx, py + 1, pstep >> 2, pack_pair(p->color));
    }
  }

  /* 6. Render the player's 3D Starfighter / Cockpit in foreground */
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
