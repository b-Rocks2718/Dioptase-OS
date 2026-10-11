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

/* Per-angle ray tables, rebuilt whenever the camera angle changes. */
static int g_table_angle = -1;
static int ray_dx[VIEW_W];
static int ray_dy[VIEW_W];
static int delta_x[VIEW_W];   /* |1 / ray_dx| in 16.16, capped */
static int delta_y[VIEW_W];

/* Perpendicular wall distance per column (16.16), for sprite occlusion. */
int g_zbuffer[VIEW_W];

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

/* Light level for a surface at perpendicular distance `dist` (16.16): the
 * player's torch lights the near field and the dungeon fades to fog. */
static int light_at(int dist){
  int level = LIGHT_FULL + 1 - (dist >> 15);
  if (level > LIGHT_FULL) level = LIGHT_FULL;
  if (level < 1) level = 1;
  return level;
}

/* Precompute each floor row's distance and light level. */
static void build_row_tables(void){
  for (int k = 0; k < VIEW_HALF_H; ++k){
    row_dist[k] = (PROJ_K << 16) / (2 * k + 1);
    row_level[k] = light_at(row_dist[k]);
  }
}

void render_init(void){
  build_row_tables();
  floor_tex = flat_textures + FLAT_FLOOR_STONE * TEX_BYTES;
  ceil_tex = flat_textures + FLAT_CEILING_BEAMS * TEX_BYTES;
}

/* Cast the floor and ceiling over the whole view; walls are drawn on top. */
static struct FlatRow flat_rows[VIEW_HALF_H];

static void draw_flats(struct Camera* cam){
  for (int k = 0; k < VIEW_HALF_H; ++k){
    struct FlatRow* row = &flat_rows[k];
    row->u = cam->x + row_u0[k];
    row->v = cam->y + row_v0[k];
    row->du = row_du[k];
    row->dv = row_dv[k];
    row->floor_shade = shade_tables + row_level[k] * SHADE_TABLE_SHORTS;
    row->ceil_shade = row->floor_shade;
  }
  draw_flats_asm(flat_rows, g_view, floor_tex, ceil_tex);
}

/* Draw the wall slice of screen column x (floor and ceiling are already cast). */
static void draw_column(int x, int cell, int dist, int tex_x, int side){
  unsigned short* col = g_view + (x >> 3) * VIEW_COL_GROUP_SHORTS + (x & 7);
  if (dist < MIN_WALL_DIST) dist = MIN_WALL_DIST;
  int height = udiv32(PROJ_K << 16, dist);
  int top = VIEW_HALF_H - (height >> 1);
  int bottom = top + height;
  int y0 = top < 0 ? 0 : top;
  int y1 = bottom > VIEW_H ? VIEW_H : bottom;

  if (y1 > y0){
    int step = udiv32(TEX_SIZE << 16, height);
    int pos = 0;
    if (top < 0){
      pos = umul16(-top, step);
    }
    int level = light_at(dist);
    if (side) level -= 1;
    unsigned char* tex = wall_textures + (cell - 1) * TEX_BYTES + tex_x * TEX_SIZE;
    draw_wall(col + y0 * VIEW_ROW_SHORTS, y1 - y0, tex, pos, step,
              shade_tables + level * SHADE_TABLE_SHORTS);
  }
}

void render_view(struct Camera* cam){
  if (cam->angle != g_table_angle){
    build_ray_tables(cam->angle);
  }
  draw_flats(cam);
  int map_x0 = cam->x >> 16;
  int map_y0 = cam->y >> 16;
  unsigned frac_x = cam->x & 0xFFFF;
  unsigned frac_y = cam->y & 0xFFFF;

  for (int x = 0; x < VIEW_W; ++x){
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
    int cell = 0;
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
      cell = g_map[cell_index & (MAP_CELLS - 1)];
      if (cell != CELL_EMPTY) break;
    }
    int dist;
    int wall_x;
    if (side == 0){
      dist = side_x - dx;
      wall_x = cam->y + fixmul(dist, ry);
    } else {
      dist = side_y - dy;
      wall_x = cam->x + fixmul(dist, rx);
    }
    int tex_x = (wall_x >> 10) & (TEX_SIZE - 1);
    if (side == 0 && rx > 0) tex_x = TEX_SIZE - 1 - tex_x;
    if (side == 1 && ry < 0) tex_x = TEX_SIZE - 1 - tex_x;
    g_zbuffer[x] = dist;
    draw_column(x, cell, dist, tex_x, side);
  }
}
