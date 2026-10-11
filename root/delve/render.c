#include "delve.h"

/*
 * Raycasting renderer for the 320x192 first-person view.
 *
 * Walls use a grid DDA per screen column (Lode Vandevenne's formulation:
 * ray direction = camera direction + camera plane * screen x, so the
 * perpendicular wall distance falls out of the side distances without a
 * fisheye correction). The ISA has no multiply or divide instruction, so the
 * per-column arithmetic is built to avoid both: multiplies are single 16x16
 * umul16 calls (gfx.s), wall heights, texture steps, and the per-ray
 * reciprocals come from tables, and ray directions step incrementally across
 * the view, so a new camera angle (the player turns freely) costs little.
 *
 * Rendering can be confined to a set of 8-pixel column groups so several
 * processes can share a frame (parallel.c); all per-frame renderer state is
 * process-local. The back buffer's layout is described in delve.h ("Screen
 * layout").
 */

static void draw_billboards(struct Camera* cam);
static int light_bonus(int x, int y, int face_side, int ray_d);

/* Everything the renderer keeps per screen column, in one record: walking
 * the columns of a strip then touches one or two pages of records instead of
 * a page in each of nine parallel arrays, which overflowed the 16-entry TLB
 * on every column. The record is 32 bytes so indexing is a shift (bcc calls
 * its software multiply for other sizes). */
struct Column {
  int ray_dx;      /* ray direction; rebuilt when the camera angle changes */
  int ray_dy;
  int delta_x;     /* |1 / ray_dx| in 16.16, capped */
  int delta_y;
  int depth;       /* perpendicular wall distance (16.16): the z-buffer */
  short tex;       /* texture index, -1 when the ray hit nothing */
  short tex_x;     /* texel column at mip level 0 */
  short height;    /* projected wall height in pixels */
  short level;     /* light level */
  int unused;      /* pads the record to 32 bytes */
};
static struct Column columns[320];       /* VIEW_W */
static int table_angle = -1;

/* Address of view pixel (x, y), computed rather than looked up in hud.c's
 * offset tables (two more pages per column): see delve.h, "Screen layout".
 * (x >> 3) * CHUNK_SHORTS is spelled with shifts (768 = 512 + 256). */
static unsigned short* view_at(int x, int y){
  int g = x >> 3;
  unsigned short* p = g_view + (g << 9) + (g << 8) + (x & 7);
  if (y >= VIEW_HALF_H) return p + HALF_SHORTS + ((y - VIEW_HALF_H) << 3);
  return p + (y << 3);
}

/* Per column group: floor/ceiling row pairs k < this are hidden behind the
 * walls of all eight of its screen columns (see draw_flats_asm). */
static int flat_skip[40];           /* VIEW_GROUPS */

/* Mip level 0 texture step (16.16 texels per pixel) for wall heights below
 * STEP_TABLE; level n steps are these shifted right by n. */
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
 * they are cached alongside the ray tables. (Bounds: VIEW_HALF_H.) */
static int row_dist[96];
static int row_level[96];
static int row_u0[96];
static int row_v0[96];
static int row_du[96];
static int row_dv[96];

/* Floor and ceiling textures for the current level. */
static unsigned char* floor_tex;
static unsigned char* ceil_tex;

/* Largest reciprocal we keep; rays more parallel than this never cross. */
#define DELTA_CAP 0x01000000

/* Reciprocals for the DDA's per-ray deltas: recip_table[i] is 1 / r in 16.16
 * for a ray component r = (16 i + 8) / 65536 (the middle of the components
 * that share the entry), capped at DELTA_CAP; the delta for a component c is
 * recip_table[|c| >> RECIP_SHIFT]. Ray components stay below
 * sqrt(1 + 0.7^2) < 1.23, inside the table's 2.0. The sampling error is under
 * 0.4% for components above 1/32; smaller ones belong to rays running nearly
 * parallel to that axis, whose crossings of it lie far away. 32 KB, built by
 * render_init() (array bounds must be literals: 8192 = 2.0 >> RECIP_SHIFT). */
#define RECIP_SHIFT 4
static int recip_table[8192];
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
 * from up to four 16x16 partial products (zero high halves are skipped).
 * Truncates toward zero. */
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
  unsigned r = umul16(al, bl) >> 16;
  if (ah != 0) r += umul16(ah, bl);
  if (bh != 0){
    r += umul16(al, bh);
    if (ah != 0) r += umul16(ah, bh) << 16;
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

/* Rebuild the per-angle tables for `angle`: each column's ray direction and
 * reciprocals, and each floor row's starting offset and per-pixel step. */
static void build_ray_tables(int angle){
  int dir_x = icos(angle);
  int dir_y = isin(angle);
  int plane_x = fixmul(-dir_y, PLANE_LEN);
  int plane_y = fixmul(dir_x, PLANE_LEN);
  /* Column x looks along dir + plane * (2x + 1 - W) / W, so the direction
   * steps by plane * 2 / W per column. The walk keeps 8 extra fraction bits
   * so rounding cannot build up across the view. */
  int wide_step_x = (plane_x << 9) / VIEW_W;
  int wide_step_y = (plane_y << 9) / VIEW_W;
  int wide_x = ((dir_x - plane_x) << 8) + (wide_step_x >> 1);
  int wide_y = ((dir_y - plane_y) << 8) + (wide_step_y >> 1);
  struct Column* col = columns;
  for (int x = 0; x < VIEW_W; ++x, ++col){
    int rx = wide_x >> 8;
    int ry = wide_y >> 8;
    col->ray_dx = rx;
    col->ray_dy = ry;
    unsigned ax = rx < 0 ? -rx : rx;
    unsigned ay = ry < 0 ? -ry : ry;
    col->delta_x = recip_table[ax >> RECIP_SHIFT];
    col->delta_y = recip_table[ay >> RECIP_SHIFT];
    wide_x += wide_step_x;
    wide_y += wide_step_y;
  }
  /* per-pixel change of the ray direction along a floor row */
  int step_x = wide_step_x >> 8;
  int step_y = wide_step_y >> 8;
  for (int k = 0; k < VIEW_HALF_H; ++k){
    int d = row_dist[k];
    row_u0[k] = fixmul(d, columns[0].ray_dx);
    row_v0[k] = fixmul(d, columns[0].ray_dy);
    row_du[k] = fixmul(d, step_x);
    row_dv[k] = fixmul(d, step_y);
  }
  table_angle = angle;
}

/* Light levels added to everything (negative during fades). */
static int light_offset;

/* The column groups this process renders this frame (own[g] != 0), and the
 * columns [strip_x0, strip_x1) those groups span. With parallel rendering
 * each process owns a strip of the view (parallel.c); the per-group test
 * also allows owned groups that are not contiguous. */
static int strip_x0;
static int strip_x1 = VIEW_W;
static unsigned char own[40];         /* VIEW_GROUPS */

/* Shift every light level by `levels` (negative darkens; used for fades). */
void set_light_offset(int levels){
  light_offset = levels;
}

/* The current light offset (published to render workers). */
int get_light_offset(void){
  return light_offset;
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
  for (int i = 0; i < 8192; ++i){
    unsigned r = udiv32(0xFFFFFFFF, (i << RECIP_SHIFT) + (1 << (RECIP_SHIFT - 1)));
    recip_table[i] = r > DELTA_CAP ? DELTA_CAP : r;
  }
  for (int h = 1; h < STEP_TABLE; ++h){
    step_for_height[h] = udiv32(WALL_SIZE << 16, h);
  }
  step_for_height[0] = WALL_SIZE << 16;
  for (int i = 0; i < HEIGHT_TABLE; ++i){
    height_for_dist[i] = udiv32(PROJ_K << 16, FIX_ONE + (i << 8));
  }
  floor_tex = flat_textures + FLAT_FLOOR_STONE * FLAT_BYTES;
  ceil_tex = flat_textures + FLAT_CEILING_BEAMS * FLAT_BYTES;
}

/* Per-row floor casting state handed to draw_flats_asm(). */
static struct FlatRow flat_rows[96];

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

/* Set each floor row's starting texture coordinates and light, then cast
 * groups [strip_x0 / 8, strip_x1 / 8). Groups this process does not own have
 * every row pair marked hidden (flat_skip), so draw_flats_asm only advances
 * their texture coordinates and draws nothing there. */
static void draw_flats(struct Camera* cam){
  int g0 = strip_x0 >> 3;
  int g1 = strip_x1 >> 3;
  for (int k = 0; k < VIEW_HALF_H; ++k){
    struct FlatRow* row = &flat_rows[k];
    /* start the row at the strip's first column */
    row->u = cam->x + row_u0[k] + row_du[k] * strip_x0;
    row->v = cam->y + row_v0[k] + row_dv[k] * strip_x0;
    row->du = row_du[k];
    row->dv = row_dv[k];
    row->level = row_level[k] + light_offset;
  }
  draw_flats_asm(flat_rows, g_view + g0 * CHUNK_SHORTS, floor_tex, ceil_tex,
                 active_lights, active_light_count, flat_skip + g0, g1 - g0);
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

/* (a * b) >> 16 for a positive distance `a` below 64 cells and a ray
 * component |b| below 4.0 (both operands then fit umul16's 16 bits after the
 * shifts), with ~10 fractional bits: plenty for a texture coordinate or a
 * light position, and one umul16 instead of a full fixmul. */
static int dist_mul(int a, int b){
  if (b < 0) return -(int)(umul16((unsigned)a >> 6, (unsigned)(-b) >> 2) >> 8);
  return umul16((unsigned)a >> 6, (unsigned)b >> 2) >> 8;
}

/* Draw the wall slice of screen column x (floor and ceiling are already
 * cast) from the raycast pass's column record. The texture's mip level is
 * picked by the column's height, switching where the level 0 step reaches
 * sqrt(2) and 2 sqrt(2) texels per pixel: near walls show every texel, and
 * far ones, which would skip most texels of a large texture and shimmer as
 * the view moves, read a smaller pre-filtered copy. */
static void draw_column(int x){
  struct Column* col = &columns[x];
  int tex = col->tex;
  if (tex < 0) return;
  int height = col->height;
  int top = VIEW_HALF_H - (height >> 1);
  int bottom = top + height;
  int y0 = top < 0 ? 0 : top;
  int y1 = bottom > VIEW_H ? VIEW_H : bottom;
  if (y1 <= y0) return;
  int step = height < STEP_TABLE ? step_for_height[height] : udiv32(WALL_SIZE << 16, height);
  unsigned char* texels = wall_textures;
  int mip = 0;
  if (height < WALL_MIP2_HEIGHT){
    texels = wall_mip2;
    mip = 2;
  } else if (height < WALL_MIP1_HEIGHT){
    texels = wall_mip1;
    mip = 1;
  }
  step >>= mip;
  /* a column taller than the view is level 0, whose step fits in 16 bits */
  int pos = 0;
  if (top < 0){
    pos = umul16(-top, step);
  }
  int level = col->level;
  if (level < 0) level = 0;
  if (level > LIGHT_LEVELS - 1) level = LIGHT_LEVELS - 1;
  /* level `mip` holds textures of (WALL_SIZE >> mip)^2 texels */
  int size_shift = WALL_SHIFT - mip;
  unsigned char* texcol = texels + (tex << (size_shift << 1)) + ((col->tex_x >> mip) << size_shift);
  draw_wall(view_at(x, y0), y1 - y0, texcol, pos, step,
            shade_tables + level * SHADE_TABLE_SHORTS, HALF_LEFT(y0));
}

/* Cast the ray for screen column x and fill in its column record and the
 * z-buffer. Marks every cell the ray passes as seen (automap). */
static void cast_column(struct Camera* cam, int x, int map_x0, int map_y0,
                        unsigned frac_x, unsigned frac_y){
  struct Column* col = &columns[x];
  int rx = col->ray_dx;
  int ry = col->ray_dy;
  int dx = col->delta_x;
  int dy = col->delta_y;
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
  col->tex = tex;
  if (tex < 0){
    col->depth = 0x7FFFFFFF;
    col->height = 0;
    return;
  }
  int tex_x = (wall_x >> (16 - WALL_SHIFT)) & (WALL_SIZE - 1);
  if (side == 0 && rx > 0) tex_x = WALL_SIZE - 1 - tex_x;
  if (side == 1 && ry < 0) tex_x = WALL_SIZE - 1 - tex_x;
  col->depth = dist;
  col->tex_x = tex_x;
  col->height = wall_height(dist);
  int level = light_at(dist) - side;
  if (active_light_count > 0){
    if (side == 0){
      level += light_bonus(cam->x + dist_mul(dist, rx), wall_x, 0, rx);
    } else {
      level += light_bonus(wall_x, cam->y + dist_mul(dist, ry), 1, ry);
    }
  }
  col->level = level;
}

/* Render the whole view (single-process rendering). */
void render_view(struct Camera* cam, struct ViewExtras* extras){
  render_view_groups(cam, extras, NULL, 0);
}

/* Draw the staff image over this process's columns, clipped to the view;
 * only each image column's opaque span is visited. */
static void draw_staff(struct ViewExtras* e){
  if (e->staff_image < 0) return;
  unsigned short* image = staff_images + e->staff_image * (STAFF_W * STAFF_H);
  unsigned char* spans = staff_spans + e->staff_image * (STAFF_W * 2);
  int x0 = e->staff_x < strip_x0 ? strip_x0 : e->staff_x;
  int x1 = e->staff_x + STAFF_W > strip_x1 ? strip_x1 : e->staff_x + STAFF_W;
  for (int x = x0; x < x1; ++x){
    if (!own[x >> 3]) continue;
    int c = x - e->staff_x;
    int first = spans[c * 2];
    int top = e->staff_y + first;
    int bottom = e->staff_y + spans[c * 2 + 1];
    if (top < 0){
      first -= top;
      top = 0;
    }
    if (bottom > VIEW_H) bottom = VIEW_H;
    if (bottom <= top) continue;
    draw_image_col(view_at(x, top), bottom - top, image + c * STAFF_H + first,
                   HALF_LEFT(top));
  }
}

/* Render the column groups g with owner[g] == me (all of them when owner is
 * NULL) of the scene seen by `cam` into g_view: cast every owned column's ray
 * (z-buffer, automap), cast the floor and ceiling except runs the walls will
 * cover, draw walls, billboards, and particles, then the staff and the
 * recolor pass from `extras`, all limited to the owned groups. */
void render_view_groups(struct Camera* cam, struct ViewExtras* extras,
                        unsigned char* owner, int me){
  int g0 = VIEW_GROUPS;
  int g1 = 0;
  for (int g = 0; g < VIEW_GROUPS; ++g){
    own[g] = owner == NULL || owner[g] == me;
    if (own[g]){
      if (g < g0) g0 = g;
      g1 = g + 1;
    }
  }
  if (g1 == 0) return;
  strip_x0 = g0 * 8;
  strip_x1 = g1 * 8;
  if (cam->angle != table_angle){
    build_ray_tables(cam->angle);
  }
  floor_tex = flat_textures + g_floor_flat * FLAT_BYTES;
  ceil_tex = flat_textures + g_ceiling_flat * FLAT_BYTES;
  select_lights(cam);
  int map_x0 = cam->x >> 16;
  int map_y0 = cam->y >> 16;
  unsigned frac_x = cam->x & 0xFFFF;
  unsigned frac_y = cam->y & 0xFFFF;
  g_seen[(map_y0 * MAP_W + map_x0) & (MAP_CELLS - 1)] = 1;

  for (int g = g0; g < g1; ++g){
    if (!own[g]) continue;
    for (int x = g * 8; x < g * 8 + 8; ++x){
      cast_column(cam, x, map_x0, map_y0, frac_x, frac_y);
    }
  }
  /* A floor/ceiling row pair k is hidden in a column when k < height / 2;
   * in groups another process renders, every pair counts as hidden. */
  for (int g = g0; g < g1; ++g){
    int skip = VIEW_HALF_H;
    if (own[g]){
      for (int x = g * 8; x < g * 8 + 8; ++x){
        int half = columns[x].height >> 1;
        if (half < skip) skip = half;
      }
    }
    flat_skip[g] = skip;
  }
  draw_flats(cam);
  for (int g = g0; g < g1; ++g){
    if (!own[g]) continue;
    for (int x = g * 8; x < g * 8 + 8; ++x){
      draw_column(x);
    }
  }
  draw_billboards(cam);
  draw_staff(extras);
  if (extras->shade_shift >= 0){
    for (int g = g0; g < g1; ++g){
      if (own[g]){
        shade_strip(g_view, g, g + 1, extras->shade_and, extras->shade_or, extras->shade_shift);
      }
    }
  }
}

/* Copy the column groups rendered by the last render_view_groups() call to
 * the pixel framebuffer. */
void render_blit_own(void){
  if (strip_x1 > strip_x0){
    blit_groups(g_pixel_fb, g_view, own, strip_x0 >> 3, strip_x1 >> 3);
  }
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
  if (right <= strip_x0 || left >= strip_x1 || bottom <= 0 || top >= VIEW_H) return;

  int step = udiv32(SPR_SIZE << 16, size_px);     /* texels per pixel */
  int x0 = left < strip_x0 ? strip_x0 : left;
  int x1 = right > strip_x1 ? strip_x1 : right;
  if (x1 <= x0) return;
  /* step exceeds 16 bits for sprites up to SPR_SIZE pixels tall, so no umul16 here */
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
    if (!own[x >> 3] || depth >= columns[x].depth) continue;
    int tc = u >> 16;
    if (tc >= SPR_SIZE) break;
    int span_top = spans[tc * 2];
    int span_bottom = spans[tc * 2 + 1];
    if (span_bottom == 0) continue;
    /* screen rows covered by the column's opaque texels */
    int y0 = top + (umul16(span_top, size_px) >> SPR_SHIFT);
    int y1 = top + ((umul16(span_bottom, size_px) + SPR_SIZE - 1) >> SPR_SHIFT);
    int v = span_top << 16;
    if (y0 < 0){
      v += step * (-y0);
      y0 = 0;
    }
    if (y1 > VIEW_H) y1 = VIEW_H;
    if (y1 <= y0) continue;
    draw_sprite_col(view_at(x, y0), y1 - y0, texels + tc * SPR_SIZE, v, step, shade,
                    HALF_LEFT(y0));
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
    if (sx < strip_x0 || sx >= strip_x1 || sy < 0 || sy >= VIEW_H) continue;
    if (!own[sx >> 3] || depth >= columns[sx].depth) continue;
    int color = p->color;
    if (p->life < 8) color = (color >> 1) & 0x0777;
    *view_at(sx, sy) = color;
    if (depth < 0x28000 && sx + 1 < strip_x1 && own[(sx + 1) >> 3] && sy + 1 < VIEW_H){
      /* closer sparks are 2x2 (the neighbors may sit in another chunk) */
      *view_at(sx + 1, sy) = color;
      *view_at(sx, sy + 1) = color;
      *view_at(sx + 1, sy + 1) = color;
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
