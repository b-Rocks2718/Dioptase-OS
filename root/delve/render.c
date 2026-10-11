#include "delve.h"

/*
 * Raycasting renderer for the 160x96 first-person view.
 *
 * Walls use a grid DDA per screen column (Lode Vandevenne's formulation:
 * ray direction = camera direction + camera plane * screen x, so the
 * perpendicular wall distance falls out of the side distances without a
 * fisheye correction). Every per-column multiply goes through umul16 in
 * gfx.s and the single division through udiv32; the expensive per-ray
 * reciprocals are cached per camera angle because the camera only turns
 * during the short turn animation.
 */

static void draw_billboards(struct Camera* cam);
static int light_bonus(int x, int y, int face_side, int ray_d);

/* Per-angle ray tables, rebuilt whenever the camera angle changes. */
static int g_table_angle = -1;
static int ray_dx[VIEW_W];
static int ray_dy[VIEW_W];
static int delta_x[VIEW_W];   /* |1 / ray_dx| in 16.16, capped */
static int delta_y[VIEW_W];

/* Perpendicular wall distance per column (16.16), for sprite occlusion. */
int g_zbuffer[VIEW_W];

/* What the raycast pass found for each column, consumed by the wall pass. */
static int col_tex[VIEW_W];      /* texture index, -1 when the ray hit nothing */
static int col_tex_x[VIEW_W];
static int col_level[VIEW_W];
static int col_height[VIEW_W];   /* projected wall height in pixels */

/* Per tile column: floor/ceiling row pairs k < this are hidden behind the
 * walls of all eight of its screen columns (see draw_flats_asm). */
static int flat_skip[VIEW_TILE_COLS];

/* Texture step (16.16 texels per pixel) for wall heights below STEP_TABLE. */
#define STEP_TABLE 1024
static int step_for_height[STEP_TABLE];

/* Wall height for distances in [1, 9) cells at 1/256-cell resolution. Closer
 * walls (where the table would be coarse) and farther ones divide. */
#define HEIGHT_TABLE 2048
static unsigned short height_for_dist[HEIGHT_TABLE];

/* Floor casting tables. Row k (0 = nearest the horizon) shows floor row
 * VIEW_HALF_H + k and ceiling row VIEW_HALF_H - 1 - k, both at distance
 * row_dist[k] = PROJ_K / (2k + 1) cells. The world offset of the row's
 * leftmost pixel and the per-pixel step depend only on the camera angle, so
 * they are cached alongside the ray tables. */
static int row_dist[VIEW_HALF_H];
static int row_level[VIEW_HALF_H];
static int row_u0[VIEW_HALF_H];
static int row_v0[VIEW_HALF_H];
static int row_du[VIEW_HALF_H];
static int row_dv[VIEW_HALF_H];

/* Floor and ceiling textures for the current level. */
static unsigned char* floor_tex;
static unsigned char* ceil_tex;

/* Largest reciprocal we keep; rays more parallel than this never cross. */
#define DELTA_CAP 0x01000000
#define MAX_RAY_STEPS 48
/* Closest a wall may be before its height computation is clamped. */
#define MIN_WALL_DIST 0x0800

/* Sine of a 12-bit angle in 16.16. */
int isin(int angle){
  return sin_table[angle & ANGLE_MASK];
}

/* Cosine of a 12-bit angle in 16.16. */
int icos(int angle){
  return sin_table[(angle + ANGLE_QUARTER) & ANGLE_MASK];
}

/* Signed 16.16 multiply: (a * b) >> 16 with a 48-bit intermediate, built
 * from four 16x16 partial products. Truncates toward zero. */
int fixmul(int a, int b){
  int negative = 0;
  unsigned ua = a;
  unsigned ub = b;
  if (a < 0){ ua = -a; negative = !negative; }
  if (b < 0){ ub = -b; negative = !negative; }
  unsigned ah = ua >> 16;
  unsigned al = ua & 0xFFFF;
  unsigned bh = ub >> 16;
  unsigned bl = ub & 0xFFFF;
  unsigned r = (umul16(al, bl) >> 16) + umul16(ah, bl) + umul16(al, bh);
  if (ah != 0 && bh != 0){
    r += umul16(ah, bh) << 16;
  }
  return negative ? -(int)r : (int)r;
}

/* (frac * delta) >> 16 for a 16.16 fraction in 0..65536 and a positive
 * delta below 2^24 (the per-ray reciprocal). */
static int frac_mul(unsigned frac, unsigned delta){
  if (frac >= FIX_ONE) return delta;
  if (frac == FIX_HALF) return delta >> 1;   /* camera at a cell center */
  return (umul16(frac, delta & 0xFFFF) >> 16) + umul16(frac, delta >> 16);
}

/* Rebuild the per-column ray directions and reciprocals for `angle`. */
static void build_ray_tables(int angle){
  int dir_x = icos(angle);
  int dir_y = isin(angle);
  int plane_x = fixmul(-dir_y, PLANE_LEN);
  int plane_y = fixmul(dir_x, PLANE_LEN);
  for (int x = 0; x < VIEW_W; ++x){
    /* camera-space x in -1..1 at the column center: (2x + 1 - W) / W */
    int cam = ((2 * x + 1 - VIEW_W) * FIX_ONE) / VIEW_W;
    int rx = dir_x + fixmul(plane_x, cam);
    int ry = dir_y + fixmul(plane_y, cam);
    ray_dx[x] = rx;
    ray_dy[x] = ry;
    unsigned ax = rx < 0 ? -rx : rx;
    unsigned ay = ry < 0 ? -ry : ry;
    delta_x[x] = ax < 256 ? DELTA_CAP : udiv32(0xFFFFFFFF, ax);
    delta_y[x] = ay < 256 ? DELTA_CAP : udiv32(0xFFFFFFFF, ay);
    if (delta_x[x] > DELTA_CAP) delta_x[x] = DELTA_CAP;
    if (delta_y[x] > DELTA_CAP) delta_y[x] = DELTA_CAP;
  }
  /* per-pixel change of the ray direction along a row: plane * 2 / W */
  int step_x = fixmul(plane_x, (2 * FIX_ONE) / VIEW_W);
  int step_y = fixmul(plane_y, (2 * FIX_ONE) / VIEW_W);
  for (int k = 0; k < VIEW_HALF_H; ++k){
    int d = row_dist[k];
    row_u0[k] = fixmul(d, ray_dx[0]);
    row_v0[k] = fixmul(d, ray_dy[0]);
    row_du[k] = fixmul(d, step_x);
    row_dv[k] = fixmul(d, step_y);
  }
  g_table_angle = angle;
}

/* Light levels added to everything (negative during fades). */
static int light_offset;

/* Shift every light level by `levels` (negative darkens; used for fades). */
void set_light_offset(int levels){
  light_offset = levels;
}

/* Light level for a surface at perpendicular distance `dist` (16.16): the
 * player's torch lights the near field and the dungeon fades to fog. */
static int light_at(int dist){
  int level = LIGHT_FULL + 1 - (dist >> 15);
  if (level > LIGHT_FULL) level = LIGHT_FULL;
  if (level < 1) level = 1;
  return level + light_offset;
}

/* Precompute each floor row's distance and light level. */
static void build_row_tables(void){
  for (int k = 0; k < VIEW_HALF_H; ++k){
    row_dist[k] = (PROJ_K << 16) / (2 * k + 1);
    row_level[k] = light_at(row_dist[k]) - light_offset;
  }
}

/* One-time renderer setup: floor row tables and default flats. */
void render_init(void){
  build_row_tables();
  for (int h = 1; h < STEP_TABLE; ++h){
    step_for_height[h] = udiv32(TEX_SIZE << 16, h);
  }
  step_for_height[0] = TEX_SIZE << 16;
  for (int i = 0; i < HEIGHT_TABLE; ++i){
    height_for_dist[i] = udiv32(PROJ_K << 16, FIX_ONE + (i << 8));
  }
  floor_tex = flat_textures + FLAT_FLOOR_STONE * TEX_BYTES;
  ceil_tex = flat_textures + FLAT_CEILING_BEAMS * TEX_BYTES;
}

/* Cast the floor and ceiling over the whole view; walls are drawn on top. */
static struct FlatRow flat_rows[VIEW_HALF_H];

/* This frame's lights that can reach anything visible: within reach of the
 * lit area (the torch fog makes surfaces past ~6 cells black anyway) and not
 * wholly behind the camera. Shared by walls, floors, and billboards. */
#define MAX_ACTIVE_LIGHTS 8
#define LIGHT_REACH 0x70000
static struct FlatLight active_lights[MAX_ACTIVE_LIGHTS];
static int active_light_count;

/* Build active_lights for this frame from g_lights (see above). */
static void select_lights(struct Camera* cam){
  int dir_x = icos(cam->angle);
  int dir_y = isin(cam->angle);
  active_light_count = 0;
  for (int i = 0; i < g_light_count && active_light_count < MAX_ACTIVE_LIGHTS; ++i){
    struct Light* l = &g_lights[i];
    int dx = l->x - cam->x;
    int dy = l->y - cam->y;
    if (approx_dist(dx, dy) > l->radius + LIGHT_REACH) continue;
    if (fixmul(dx, dir_x) + fixmul(dy, dir_y) < -l->radius) continue;
    struct FlatLight* a = &active_lights[active_light_count++];
    a->x = l->x;
    a->y = l->y;
    a->radius = l->radius;
  }
}

/* Set each floor row's starting texture coordinates and light, then cast. */
static void draw_flats(struct Camera* cam){
  for (int k = 0; k < VIEW_HALF_H; ++k){
    struct FlatRow* row = &flat_rows[k];
    row->u = cam->x + row_u0[k];
    row->v = cam->y + row_v0[k];
    row->du = row_du[k];
    row->dv = row_dv[k];
    row->level = row_level[k] + light_offset;
  }
  draw_flats_asm(flat_rows, g_view, floor_tex, ceil_tex, active_lights,
                 active_light_count, flat_skip);
}

/* Extra light levels at world point (x, y) from this frame's point lights.
 * With `face_side` >= 0 the point is on a wall face and only lights on the
 * viewer's side of that face count: face_side 0 is an x face seen by a ray
 * with x direction `ray_d`, 1 a y face. */
static int light_bonus(int x, int y, int face_side, int ray_d){
  int bonus = 0;
  for (int i = 0; i < active_light_count; ++i){
    struct FlatLight* l = &active_lights[i];
    if (face_side == 0 && ((l->x - x) ^ ray_d) >= 0) continue;
    if (face_side == 1 && ((l->y - y) ^ ray_d) >= 0) continue;
    int d = approx_dist(x - l->x, y - l->y);
    if (d < l->radius){
      bonus += (l->radius - d) >> 15;
    }
  }
  return bonus;
}

/* Projected wall height in pixels for perpendicular distance `dist`. */
static int wall_height(int dist){
  if (dist < MIN_WALL_DIST) dist = MIN_WALL_DIST;
  unsigned i = (unsigned)(dist - FIX_ONE) >> 8;
  if (dist >= FIX_ONE && i < HEIGHT_TABLE) return height_for_dist[i];
  return udiv32(PROJ_K << 16, dist);
}

/* (a * b) >> 16 for a positive distance `a` below 32 cells and a ray
 * component |b| below 2.0, with ~10 fractional bits: plenty for a texture
 * coordinate or a light position, and one umul16 instead of a full fixmul. */
static int dist_mul(int a, int b){
  if (b < 0) return -(int)(umul16((unsigned)a >> 6, (unsigned)(-b) >> 2) >> 8);
  return umul16((unsigned)a >> 6, (unsigned)b >> 2) >> 8;
}

/* Draw the wall slice of screen column x (floor and ceiling are already
 * cast) from the raycast pass's column record. */
static void draw_column(int x){
  int tex = col_tex[x];
  if (tex < 0) return;
  int height = col_height[x];
  unsigned short* col = g_view + (x >> 3) * VIEW_COL_GROUP_SHORTS + (x & 7);
  int top = VIEW_HALF_H - (height >> 1);
  int bottom = top + height;
  int y0 = top < 0 ? 0 : top;
  int y1 = bottom > VIEW_H ? VIEW_H : bottom;
  if (y1 <= y0) return;
  int step = height < STEP_TABLE ? step_for_height[height] : udiv32(TEX_SIZE << 16, height);
  int pos = 0;
  if (top < 0){
    pos = umul16(-top, step);
  }
  int level = col_level[x];
  if (level < 0) level = 0;
  if (level > LIGHT_LEVELS - 1) level = LIGHT_LEVELS - 1;
  unsigned char* texcol = wall_textures + tex * TEX_BYTES + col_tex_x[x] * TEX_SIZE;
  draw_wall(col + y0 * VIEW_ROW_SHORTS, y1 - y0, texcol, pos, step,
            shade_tables + level * SHADE_TABLE_SHORTS);
}

/* Cast the ray for screen column x and fill in its column record and the
 * z-buffer. Marks every cell the ray passes as seen (automap). */
static void cast_column(struct Camera* cam, int x, int map_x0, int map_y0,
                        unsigned frac_x, unsigned frac_y){
  int rx = ray_dx[x];
  int ry = ray_dy[x];
  int dx = delta_x[x];
  int dy = delta_y[x];
  int step_x;
  int step_y;
  int side_x;
  int side_y;
  if (rx < 0){
    step_x = -1;
    side_x = frac_mul(frac_x, dx);
  } else {
    step_x = 1;
    side_x = frac_mul(FIX_ONE - frac_x, dx);
  }
  if (ry < 0){
    step_y = -MAP_W;
    side_y = frac_mul(frac_y, dy);
  } else {
    step_y = MAP_W;
    side_y = frac_mul(FIX_ONE - frac_y, dy);
  }
  int cell_index = map_y0 * MAP_W + map_x0;
  int side = 0;
  int dist = 0;
  int wall_x = 0;
  int tex = -1;
  for (int i = 0; i < MAX_RAY_STEPS; ++i){
    if (side_x < side_y){
      side_x += dx;
      cell_index += step_x;
      side = 0;
    } else {
      side_y += dy;
      cell_index += step_y;
      side = 1;
    }
    cell_index &= MAP_CELLS - 1;
    int cell = g_map[cell_index];
    g_seen[cell_index] = 1;
    if (cell == CELL_EMPTY) continue;
    if (cell < CELL_DOOR_BASE){
      tex = cell - 1;
      if (side == 0){
        dist = side_x - dx;
        wall_x = cam->y + dist_mul(dist, ry);
      } else {
        dist = side_y - dy;
        wall_x = cam->x + dist_mul(dist, rx);
      }
      break;
    }
    /* Door: a thin panel on the cell's center plane. The ray crosses that
     * plane half a cell before its next grid line on the door's axis. */
    struct Door* door = &g_doors[cell - CELL_DOOR_BASE];
    int t_enter = side == 0 ? side_x - dx : side_y - dy;
    int t_mid;
    int along;
    if (door->axis_x){
      t_mid = side_x - (dx >> 1);
      if (t_mid < t_enter || t_mid > side_y) continue;
      along = cam->y + dist_mul(t_mid, ry);
    } else {
      t_mid = side_y - (dy >> 1);
      if (t_mid < t_enter || t_mid > side_x) continue;
      along = cam->x + dist_mul(t_mid, rx);
    }
    if ((along & 0xFFFF) < door->open) continue;   /* through the gap */
    dist = t_mid;
    wall_x = along - door->open;
    side = door->axis_x ? 0 : 1;
    tex = door->locked ? TEX_DOOR_LOCKED : TEX_DOOR;
    break;
  }
  col_tex[x] = tex;
  if (tex < 0){
    g_zbuffer[x] = 0x7FFFFFFF;
    col_height[x] = 0;
    return;
  }
  int tex_x = (wall_x >> 10) & (TEX_SIZE - 1);
  if (side == 0 && rx > 0) tex_x = TEX_SIZE - 1 - tex_x;
  if (side == 1 && ry < 0) tex_x = TEX_SIZE - 1 - tex_x;
  g_zbuffer[x] = dist;
  col_tex_x[x] = tex_x;
  col_height[x] = wall_height(dist);
  int level = light_at(dist) - side;
  if (active_light_count > 0){
    if (side == 0){
      level += light_bonus(cam->x + dist_mul(dist, rx), wall_x, 0, rx);
    } else {
      level += light_bonus(wall_x, cam->y + dist_mul(dist, ry), 1, ry);
    }
  }
  col_level[x] = level;
}

/* Render the scene seen by `cam` into g_view in three passes: cast every
 * column's ray (z-buffer, automap), cast the floor and ceiling except runs
 * the walls will cover, then draw walls, billboards, and particles. */
void render_view(struct Camera* cam){
  if (cam->angle != g_table_angle){
    build_ray_tables(cam->angle);
  }
  floor_tex = flat_textures + g_floor_flat * TEX_BYTES;
  ceil_tex = flat_textures + g_ceiling_flat * TEX_BYTES;
  select_lights(cam);
  int map_x0 = cam->x >> 16;
  int map_y0 = cam->y >> 16;
  unsigned frac_x = cam->x & 0xFFFF;
  unsigned frac_y = cam->y & 0xFFFF;
  g_seen[(map_y0 * MAP_W + map_x0) & (MAP_CELLS - 1)] = 1;

  for (int x = 0; x < VIEW_W; ++x){
    cast_column(cam, x, map_x0, map_y0, frac_x, frac_y);
  }
  /* A floor/ceiling row pair k is hidden in a column when k < height / 2. */
  for (int g = 0; g < VIEW_TILE_COLS; ++g){
    int skip = VIEW_HALF_H;
    for (int x = g * 8; x < g * 8 + 8; ++x){
      int half = col_height[x] >> 1;
      if (half < skip) skip = half;
    }
    flat_skip[g] = skip;
  }
  draw_flats(cam);
  for (int x = 0; x < VIEW_W; ++x){
    draw_column(x);
  }
  draw_billboards(cam);
}

/* ---- Billboards ---- */

struct Billboard g_bills[MAX_BILLBOARDS];
int g_bill_count;
int g_debug_bills;     /* billboards queued last frame (fps overlay) */
int g_debug_visible;   /* of those, in front of the camera */
static struct Billboard* bill_order[MAX_BILLBOARDS];

/* Queue a sprite for this frame; returns NULL when the list is full. */
struct Billboard* add_billboard(int x, int y, int z, int size, int frame){
  if (g_bill_count >= MAX_BILLBOARDS) return NULL;
  struct Billboard* b = &g_bills[g_bill_count++];
  b->x = x;
  b->y = y;
  b->z = z;
  b->size = size;
  b->frame = frame;
  b->light = 0;
  b->flags = 0;
  return b;
}

/* Project and draw one billboard against the wall z-buffer. */
static void draw_billboard(struct Camera* cam, struct Billboard* b,
                           int dir_x, int dir_y){
  int rel_x = b->x - cam->x;
  int rel_y = b->y - cam->y;
  int depth = b->depth;
  /* lateral offset along the camera's right vector (-dir_y, dir_x) */
  int lateral = fixmul(rel_y, dir_x) - fixmul(rel_x, dir_y);
  int inv = udiv32(PROJ_K << 16, depth);          /* pixels per world unit */
  int center_x = VIEW_W / 2 + fixmul(lateral, inv);
  int size_px = fixmul(b->size, inv);
  if (size_px < 1) return;
  int bottom = VIEW_HALF_H + fixmul(FIX_HALF - b->z, inv);
  int top = bottom - size_px;
  int left = center_x - (size_px >> 1);
  int right = left + size_px;
  if (right <= 0 || left >= VIEW_W || bottom <= 0 || top >= VIEW_H) return;

  int step = udiv32(SPR_SIZE << 16, size_px);     /* texels per pixel */
  int x0 = left < 0 ? 0 : left;
  int x1 = right > VIEW_W ? VIEW_W : right;
  /* step exceeds 16 bits for sprites under 32 pixels, so no umul16 here */
  int u = step * (x0 - left);

  int level;
  if (b->flags & BB_FLASH){
    level = LIGHT_LEVELS - 1;
  } else if (b->flags & BB_FULLBRIGHT){
    level = LIGHT_FULL + light_offset;
  } else {
    level = light_at(depth) + b->light;
    if (active_light_count > 0) level += light_bonus(b->x, b->y, -1, 0);
  }
  if (level < 0) level = 0;
  if (level > LIGHT_LEVELS - 1) level = LIGHT_LEVELS - 1;
  unsigned short* shade = shade_tables + level * SHADE_TABLE_SHORTS;
  unsigned char* texels = sprite_texels + b->frame * SPR_BYTES;
  unsigned char* spans = sprite_spans + b->frame * (SPR_SIZE * 2);

  for (int x = x0; x < x1; ++x, u += step){
    if (depth >= g_zbuffer[x]) continue;
    int tc = u >> 16;
    if (tc >= SPR_SIZE) break;
    int span_top = spans[tc * 2];
    int span_bottom = spans[tc * 2 + 1];
    if (span_bottom == 0) continue;
    /* screen rows covered by the column's opaque texels */
    int y0 = top + (umul16(span_top, size_px) >> 5);
    int y1 = top + ((umul16(span_bottom, size_px) + 31) >> 5);
    int v = span_top << 16;
    if (y0 < 0){
      v += step * (-y0);
      y0 = 0;
    }
    if (y1 > VIEW_H) y1 = VIEW_H;
    if (y1 <= y0) continue;
    unsigned short* dst = g_view + (x >> 3) * VIEW_COL_GROUP_SHORTS + (x & 7) + y0 * VIEW_ROW_SHORTS;
    draw_sprite_col(dst, y1 - y0, texels + tc * SPR_SIZE, v, step, shade);
  }
}

/* Draw the live spark particles as one- or two-pixel points. */
static void draw_particles(struct Camera* cam, int dir_x, int dir_y){
  for (int i = 0; i < MAX_PARTICLES; ++i){
    struct Particle* p = &g_particles[i];
    if (p->life <= 0) continue;
    int dx = p->x - cam->x;
    int dy = p->y - cam->y;
    int depth = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    if (depth < 0x3000) continue;
    int lateral = fixmul(dy, dir_x) - fixmul(dx, dir_y);
    int inv = udiv32(PROJ_K << 16, depth);
    int sx = VIEW_W / 2 + fixmul(lateral, inv);
    int sy = VIEW_HALF_H + fixmul(FIX_HALF - p->z, inv);
    if (sx < 0 || sx >= VIEW_W || sy < 0 || sy >= VIEW_H) continue;
    if (depth >= g_zbuffer[sx]) continue;
    int color = p->color;
    if (p->life < 8) color = (color >> 1) & 0x0777;
    unsigned short* px = g_view + (sx >> 3) * VIEW_COL_GROUP_SHORTS + (sx & 7) + sy * VIEW_ROW_SHORTS;
    *px = color;
    if (depth < 0x18000 && sx + 1 < VIEW_W && sy + 1 < VIEW_H){
      /* close sparks are 2x2; (sx + 1) may cross into the next tile column */
      unsigned short* right = g_view + ((sx + 1) >> 3) * VIEW_COL_GROUP_SHORTS + ((sx + 1) & 7) + sy * VIEW_ROW_SHORTS;
      right[0] = color;
      px[VIEW_ROW_SHORTS] = color;
      right[VIEW_ROW_SHORTS] = color;
    }
  }
}

/* Draw every queued billboard far-to-near, then clear the queue. */
static void draw_billboards(struct Camera* cam){
  int dir_x = icos(cam->angle);
  int dir_y = isin(cam->angle);
  int n = 0;
  for (int i = 0; i < g_bill_count; ++i){
    struct Billboard* b = &g_bills[i];
    b->depth = fixmul(b->x - cam->x, dir_x) + fixmul(b->y - cam->y, dir_y);
    if (b->depth < 0x2000) continue;   /* behind or inside the camera */
    /* insertion sort by decreasing depth */
    int j = n;
    while (j > 0 && bill_order[j - 1]->depth < b->depth){
      bill_order[j] = bill_order[j - 1];
      j -= 1;
    }
    bill_order[j] = b;
    n += 1;
  }
  for (int i = 0; i < n; ++i){
    draw_billboard(cam, bill_order[i], dir_x, dir_y);
  }
  draw_particles(cam, dir_x, dir_y);
  g_debug_bills = g_bill_count;
  g_debug_visible = n;
  g_bill_count = 0;
}
