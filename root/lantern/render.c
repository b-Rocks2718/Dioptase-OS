#include "lantern.h"
#include "../crt/sys.h"
#include "../crt/vga.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/print.h"

/* One cell in front of the camera fills about two thirds of the view. */
#define PROJECTION ((VIEW_H * 512) / 3)

int lantern_dir_x;
int lantern_dir_y;
int lantern_right_x;
int lantern_right_y;

static int sintab[ANGLE_FULL];
static short textures[10240];
static short sprite_art[1024];
static short ceil_row[VIEW_H];
static short floor_row[VIEW_H];
static short* wallbuf;
static short* view;
static short* shadow;
static int col_perp[VIEW_W];
static volatile short* pixels;
static volatile short* tiles;
static volatile short* sprites;
static int last_x;
static int last_y;
static int last_angle;
static int have_walls;
static int hud_mode;

/* Quarter-wave sine, 0 through 90 degrees, in 8.8. Index 64 is 256. */
static int quarter_sin[65] = {
  0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92,
  98, 104, 109, 115, 121, 126, 132, 137, 142, 147, 152, 157, 162, 167, 172, 177,
  181, 185, 190, 194, 198, 202, 206, 209, 213, 216, 220, 223, 226, 229, 231, 234,
  237, 239, 241, 243, 245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256,
  256
};

/* Wrap an angle into 0..1023 without relying on signed masking. */
static int wrap_angle(int angle) {
  while (angle < 0) angle += ANGLE_FULL;
  while (angle >= ANGLE_FULL) angle -= ANGLE_FULL;
  return angle;
}

/* Sine of an angle in 8.8. Cosine is sine shifted by a quarter turn. */
static int sin_fp(int angle) {
  return sintab[wrap_angle(angle)];
}

void render_sync_facing(void) {
  lantern_dir_x = sin_fp(player_angle + ANGLE_RIGHT);
  lantern_dir_y = sin_fp(player_angle);
  lantern_right_x = sin_fp(player_angle + ANGLE_RIGHT + ANGLE_RIGHT);
  lantern_right_y = sin_fp(player_angle + ANGLE_RIGHT);
}

/* Expand the quarter table into a full sine. */
static void init_trig(void) {
  int rise[256];
  int q;
  for (q = 0; q < 256; q++) {
    int i = q >> 2;
    int frac = q & 3;
    int s0 = quarter_sin[i];
    int s1 = quarter_sin[i + 1];
    rise[q] = s0 + (((s1 - s0) * frac) >> 2);
  }
  for (q = 0; q < 256; q++) {
    sintab[q] = rise[q];
    sintab[256 + q] = rise[255 - q];
    sintab[512 + q] = -rise[q];
    sintab[768 + q] = -rise[255 - q];
  }
}

/* Darken one 12-bit 0x0BGR color. shade 7 is the source; 0 is dim. */
static short shade_color(int color, int shade) {
  int r = (color >> 8) & 15;
  int g = (color >> 4) & 15;
  int b = color & 15;
  int scale = shade + 2;
  r = (r * scale) >> 3;
  g = (g * scale) >> 3;
  b = (b * scale) >> 3;
  if (r > 15) r = 15;
  if (g > 15) g = 15;
  if (b > 15) b = 15;
  return (short)((r << 8) | (g << 4) | b);
}

/* Unshaded 16x16 maps. Shading copies out of this so darker levels do not feed each other. */
static short base_tex[1280];

/* Build the five wall textures and eight distance shades. */
static void init_textures(void) {
  int tex;
  int y;
  int x;
  for (y = 0; y < 16; y++) {
    for (x = 0; x < 16; x++) {
      int stone = 0xAAA;
      int brick = 0xC64;
      int metal = 0xBBB;
      int door = 0xA74;
      int gate = 0x414;
      int n = (x * 17 + y * 13) ^ (x << 3);
      int offset = ((y >> 3) & 1) << 2;
      if ((n & 7) == 0) stone = 0x666;
      else if ((n & 7) == 1) stone = 0xDDD;
      if ((y & 7) == 0 || ((x + offset) & 7) == 0) brick = 0x421;
      if ((y % 5) == 0) metal = 0x666;
      if ((x & 7) == 2 && (y % 5) == 2) metal = 0xEEE;
      if ((x & 3) == 0) door = 0x421;
      if (y >= 6 && y <= 9 && x >= 2 && x <= 13) door = 0xEE4;
      if ((x & 3) == 0 || (y & 3) == 0) gate = 0xF3F;
      base_tex[y * 16 + x] = (short)stone;
      base_tex[256 + y * 16 + x] = (short)brick;
      base_tex[512 + y * 16 + x] = (short)metal;
      base_tex[768 + y * 16 + x] = (short)door;
      base_tex[1024 + y * 16 + x] = (short)gate;
    }
  }
  for (tex = 0; tex < 5; tex++) {
    int shade;
    for (shade = 0; shade < 8; shade++) {
      for (y = 0; y < 256; y++) {
        textures[(tex * 8 + shade) * 256 + y] = shade_color(base_tex[tex * 256 + y], shade);
      }
    }
  }
}

/* Ceiling is dark blue, brighter overhead. Floor is brown, darker at the horizon.
 * Each row is one flat color, so turning does not rewrite the floor or ceiling. */
static void init_gradients(void) {
  int y;
  int span = VIEW_H / 2;
  for (y = 0; y < VIEW_H; y++) {
    if (y < span) {
      int d = (span - 1) - y;
      int b = 2 + (d * 7) / span;
      int g = (d * 2) / span;
      ceil_row[y] = (short)((g << 4) | b);
    } else {
      int d = y - span;
      int r = 2 + (d * 7) / span;
      int g = 1 + (d * 3) / span;
      if (r > 11) r = 11;
      if (g > 6) g = 6;
      floor_row[y] = (short)((r << 8) | (g << 4) | 1);
    }
  }
}

/* 16x16 silhouettes. '.' is transparent. D is the dark outline. */
static char* sentry_rows[16] = {
  "................",
  "......DDDD......",
  ".....DRRRRD.....",
  "....DRRRRRRD....",
  "...DRRWWRRWWD...",
  "...DRRWWRRWWD...",
  "....DRRDDDRD....",
  ".....DRRRRD.....",
  "....DRRRRRRD....",
  "...DRRRRRRRRD...",
  "....DRR..RRD....",
  "....DR....RD....",
  "....DR....RD....",
  "....DD....DD....",
  "................",
  "................"
};

static char* core_rows[16] = {
  "................",
  "................",
  ".......OO.......",
  ".....OOOOOO.....",
  "....OOOWWOOO....",
  "...OOOWWWWWOO...",
  "...OOOWWWWWOO...",
  "....OOOWWOOO....",
  ".....OOOOOO.....",
  ".......OO.......",
  "................",
  "................",
  "................",
  "................",
  "................",
  "................"
};

static char* health_rows[16] = {
  "................",
  "................",
  "......DDDD......",
  ".....DGGGGD.....",
  ".....DGGGGD.....",
  "..DDDDGGGGDDDD..",
  "..DGGGGGGGGGGD..",
  "..DGGGGGGGGGGD..",
  "..DDDDGGGGDDDD..",
  ".....DGGGGD.....",
  ".....DGGGGD.....",
  "......DDDD......",
  "................",
  "................",
  "................",
  "................"
};

static char* crystal_rows[16] = {
  "................",
  "................",
  ".......CC.......",
  "......CWWC......",
  ".....CWWWWC.....",
  "....CWWWWWWC....",
  "...CWWWWWWWWC...",
  "....CWWWWWWC....",
  ".....CWWWWC.....",
  "......CWWC......",
  ".......CC.......",
  "................",
  "................",
  "................",
  "................",
  "................"
};

/* Map one silhouette letter to a 12-bit 0x0BGR color. Anything else is clear. */
static short art_color(char ch) {
  if (ch == 'R') return (short)0xE22;
  if (ch == 'O') return (short)0xC3C;
  if (ch == 'G') return (short)0x2E4;
  if (ch == 'C') return (short)0x2EE;
  if (ch == 'W') return (short)0xEEE;
  if (ch == 'D') return (short)0x111;
  return (short)0xF000;
}

/* Copy one 16x16 silhouette into the billboard table. */
static void paint_rows(int kind, char** rows) {
  int y;
  short* dst = sprite_art + kind * 256;
  for (y = 0; y < 16; y++) {
    int x;
    for (x = 0; x < 16; x++) {
      dst[y * 16 + x] = art_color(rows[y][x]);
    }
  }
}

/* Fill one 32x32 hardware sprite, then leave the rest transparent. */
static void clear_sprite(int slot) {
  int i;
  for (i = 0; i < 1024; i++) {
    sprites[slot * 1024 + i] = (short)0xF000;
  }
}

/* Draw the handheld lantern into its sprite slot. glow selects the flame. */
static void paint_lantern_sprite(int glow) {
  int y;
  int x;
  volatile short* dst = sprites + SPRITE_LANTERN * 1024;
  short flame = (glow & 1) ? (short)0xFF6 : (short)0xEC2;
  for (y = 0; y < 32; y++) {
    for (x = 0; x < 32; x++) {
      short color = (short)0xF000;
      if (x >= 14 && x <= 17 && y >= 22 && y <= 30) color = (short)0x632;
      if (x >= 11 && x <= 20 && y >= 8 && y <= 22) color = (short)0x555;
      if (x >= 13 && x <= 18 && y >= 10 && y <= 20) color = flame;
      if (x >= 12 && x <= 19 && y >= 6 && y <= 8) color = (short)0x888;
      dst[y * 32 + x] = color;
    }
  }
}

/* Crosshair with a hole in the middle so the target stays visible. */
static void paint_crosshair(void) {
  int i;
  volatile short* dst = sprites + SPRITE_CROSS * 1024;
  for (i = 8; i <= 23; i++) {
    if (i < 14 || i > 17) {
      dst[15 * 32 + i] = (short)0xDDD;
      dst[i * 32 + 15] = (short)0xDDD;
    }
  }
}

/* Muzzle flash, shown by moving the sprite on screen for a few frames. */
static void paint_muzzle(void) {
  int y;
  int x;
  volatile short* dst = sprites + SPRITE_MUZZLE * 1024;
  for (y = 0; y < 32; y++) {
    for (x = 0; x < 32; x++) {
      int dx = x - 16;
      int dy = y - 16;
      if (dx * dx + dy * dy < 36) dst[y * 32 + x] = (short)0xFF8;
    }
  }
}

/* Cast one column into the wall buffer and record its perpendicular depth. */
static void cast_column(int col) {
  int ang = wrap_angle(player_angle + (col - VIEW_CENTER) * COL_STEP);
  int dir_x = sin_fp(ang + ANGLE_RIGHT);
  int dir_y = sin_fp(ang);
  int adx = dir_x < 0 ? -dir_x : dir_x;
  int ady = dir_y < 0 ? -dir_y : dir_y;
  int delta_x = adx < 1 ? 2000000000 : 65536 / adx;
  int delta_y = ady < 1 ? 2000000000 : 65536 / ady;
  int map_x = player_x >> 8;
  int map_y = player_y >> 8;
  int frac_x = player_x & 255;
  int frac_y = player_y & 255;
  int step_x = dir_x < 0 ? -1 : 1;
  int step_y = dir_y < 0 ? -1 : 1;
  int side_x = 2000000000;
  int side_y = 2000000000;
  int side = 0;
  int along = 1;
  int perp = 1;
  int hit_cell = CELL_STONE;
  int guard;
  int rel;
  int line;
  int y0;
  int y1;
  int tex = 0;
  int shade;
  int tex_u;
  int tex_v;
  int vstep;
  int y;

  if (adx >= 1) {
    if (dir_x < 0) side_x = (frac_x * delta_x) >> 8;
    else side_x = ((256 - frac_x) * delta_x) >> 8;
  }
  if (ady >= 1) {
    if (dir_y < 0) side_y = (frac_y * delta_y) >> 8;
    else side_y = ((256 - frac_y) * delta_y) >> 8;
  }

  for (guard = 0; guard < 64; guard++) {
    if (side_x < side_y) {
      side_x += delta_x;
      map_x += step_x;
      side = 0;
    } else {
      side_y += delta_y;
      map_y += step_y;
      side = 1;
    }
    if (map_x < 0 || map_y < 0 || map_x >= MAP_W || map_y >= MAP_H) {
      hit_cell = CELL_STONE;
      break;
    }
    hit_cell = cells[map_y][map_x];
    if (hit_cell == CELL_STONE || hit_cell == CELL_BRICK || hit_cell == CELL_METAL ||
        hit_cell == CELL_DOOR || hit_cell == CELL_GATE) {
      break;
    }
  }

  along = (side == 0) ? (side_x - delta_x) : (side_y - delta_y);
  if (along < 1) along = 1;
  if (along > 16000) along = 16000;
  /* Euclidean distance bows the walls. Multiply by cos of the column offset. */
  rel = (col - VIEW_CENTER) * COL_STEP;
  if (rel < 0) rel = -rel;
  perp = (along * sin_fp(rel + ANGLE_RIGHT)) >> 8;
  if (perp < 1) perp = 1;
  col_perp[col] = perp;

  if (hit_cell == CELL_BRICK) tex = 1;
  else if (hit_cell == CELL_METAL) tex = 2;
  else if (hit_cell == CELL_DOOR) tex = 3;
  else if (hit_cell == CELL_GATE) tex = 4;

  shade = 7;
  if (perp > 450) shade = 6;
  if (perp > 800) shade = 5;
  if (perp > 1200) shade = 4;
  if (perp > 1700) shade = 3;
  if (perp > 2300) shade = 2;
  if (perp > 3200) shade = 1;
  if (perp > 4200) shade = 0;
  if (side && shade > 0) shade--;

  if (side == 0) tex_u = (player_y + ((along * dir_y) >> 8)) & 255;
  else tex_u = (player_x + ((along * dir_x) >> 8)) & 255;
  tex_u = tex_u >> 4;
  if (side == 0 && dir_x > 0) tex_u = 15 - tex_u;
  if (side == 1 && dir_y < 0) tex_u = 15 - tex_u;

  line = PROJECTION / perp;
  if (line > VIEW_H) line = VIEW_H;
  if (line < 1) line = 1;
  y0 = (VIEW_H - line) >> 1;
  y1 = y0 + line;
  vstep = (16 << 8) / line;
  tex_v = 0;
  for (y = 0; y < VIEW_H; y++) {
    short color;
    if (y < y0) color = ceil_row[y];
    else if (y >= y1) color = floor_row[y];
    else {
      int tv = tex_v >> 8;
      if (tv > 15) tv = 15;
      color = textures[(tex * 8 + shade) * 256 + tv * 16 + tex_u];
      tex_v += vstep;
    }
    wallbuf[y * VIEW_W + col] = color;
  }
}

/* Slide a pure rotation by whole columns and cast only the new edge. */
static void shift_walls(int delta) {
  int y;
  int x;
  if (delta > 0) {
    for (y = 0; y < VIEW_H; y++) {
      short* row = wallbuf + y * VIEW_W;
      for (x = 0; x < VIEW_W - delta; x++) row[x] = row[x + delta];
    }
    for (x = 0; x < VIEW_W - delta; x++) col_perp[x] = col_perp[x + delta];
    for (x = VIEW_W - delta; x < VIEW_W; x++) cast_column(x);
  } else {
    int n = -delta;
    for (y = 0; y < VIEW_H; y++) {
      short* row = wallbuf + y * VIEW_W;
      for (x = VIEW_W - 1; x >= n; x--) row[x] = row[x - n];
    }
    for (x = VIEW_W - 1; x >= n; x--) col_perp[x] = col_perp[x - n];
    for (x = 0; x < n; x++) cast_column(x);
  }
}

/* Recast the view, or rotate the previous one when only the angle changed. */
static void rebuild_walls(void) {
  int delta = player_angle - last_angle;
  int moved = player_x != last_x || player_y != last_y || world_dirty || !have_walls;
  int cols;
  int col;
  if (delta > 512) delta -= ANGLE_FULL;
  if (delta < -512) delta += ANGLE_FULL;
  /* A pure turn slides by whole columns only when the step matches COL_STEP. */
  cols = (delta % COL_STEP) == 0 ? delta / COL_STEP : VIEW_W;
  if (moved || cols <= -VIEW_W || cols >= VIEW_W) {
    for (col = 0; col < VIEW_W; col++) cast_column(col);
  } else if (cols != 0) {
    shift_walls(cols);
    cast_column(VIEW_CENTER);
  }
  last_x = player_x;
  last_y = player_y;
  last_angle = player_angle;
  world_dirty = 0;
  have_walls = 1;
}

/* Draw one billboard if it sits in front of the wall in that column. */
static void blit_sprite(int kind, int left, int top, int size, int depth, int hurt) {
  int step;
  int acc_y = 0;
  int py;
  if (size < 2) return;
  step = (16 << 8) / size;
  for (py = 0; py < size; py++) {
    int ty = acc_y >> 8;
    int vy = top + py;
    int acc_x = 0;
    int px;
    if (ty > 15) ty = 15;
    if (vy >= 0 && vy < VIEW_H) {
      for (px = 0; px < size; px++) {
        int tx = acc_x >> 8;
        int vx = left + px;
        if (tx > 15) tx = 15;
        if (vx >= 0 && vx < VIEW_W && depth < col_perp[vx]) {
          short color = sprite_art[kind * 256 + ty * 16 + tx];
          if ((color & 0xF000) != 0xF000) {
            if (hurt && ((px + py) & 1)) color = (short)0xF44;
            view[vy * VIEW_W + vx] = color;
          }
        }
        acc_x += step;
      }
    }
    acc_y += step;
  }
}

/* Place one billboard. Returns 0 when it is behind the camera or only a speck. */
static int project_sprite(int wx, int wy, int kind, int* left, int* top, int* size, int* depth) {
  int dx = wx - player_x;
  int dy = wy - player_y;
  int rel_r = (dx * lantern_right_x + dy * lantern_right_y) >> 8;
  int rel_f = (dx * lantern_dir_x + dy * lantern_dir_y) >> 8;
  int num = (PROJECTION * 3) / 4;
  int sprite_h;
  int wall;
  int scol;
  if (kind == KIND_HEALTH) num = PROJECTION / 2;
  if (kind == KIND_CORE || kind == KIND_CRYSTAL) num = (PROJECTION * 5) / 6;
  if (rel_f < 24 || rel_f > 5000) return 0;
  sprite_h = num / rel_f;
  if (sprite_h < 6) return 0;
  if (sprite_h > VIEW_H - 8) sprite_h = VIEW_H - 8;
  wall = PROJECTION / rel_f;
  scol = VIEW_CENTER + (rel_r * 163) / (rel_f * COL_STEP);
  *left = scol - sprite_h / 2;
  *top = VIEW_H / 2 + wall / 2 - sprite_h;
  *size = sprite_h;
  *depth = rel_f;
  return 1;
}

/* Project actors and the exit crystal, far sprites first. */
static void draw_sprites(void) {
  int order[17];
  int depthv[17];
  int kindv[17];
  int leftv[17];
  int topv[17];
  int sizev[17];
  int hurtv[17];
  int n = 0;
  int i;

  for (i = 0; i < actor_count; i++) {
    struct Actor* actor = &actors[i];
    int left;
    int top;
    int size;
    int depth;
    if (!actor->alive) continue;
    if (!project_sprite(actor->x, actor->y, actor->kind, &left, &top, &size, &depth)) continue;
    order[n] = n;
    depthv[n] = depth;
    kindv[n] = actor->kind;
    sizev[n] = size;
    leftv[n] = left;
    topv[n] = top;
    hurtv[n] = actor->hurt;
    n++;
  }

  if (n < MAX_ACTORS + 1) {
    int left;
    int top;
    int size;
    int depth;
    int wx = (exit_x << 8) + 128;
    int wy = (exit_y << 8) + 128;
    if (project_sprite(wx, wy, KIND_CRYSTAL, &left, &top, &size, &depth)) {
      order[n] = n;
      depthv[n] = depth;
      kindv[n] = KIND_CRYSTAL;
      sizev[n] = size;
      leftv[n] = left;
      topv[n] = top;
      hurtv[n] = 0;
      n++;
    }
  }

  for (i = 1; i < n; i++) {
    int key = order[i];
    int j = i;
    while (j > 0 && depthv[order[j - 1]] < depthv[key]) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }
  for (i = 0; i < n; i++) {
    int s = order[i];
    blit_sprite(kindv[s], leftv[s], topv[s], sizev[s], depthv[s], hurtv[s]);
  }
}

/* The whole vault in the corner. One cell is one view pixel, and it fits at 80 wide. */
static void draw_minimap(void) {
  int ox = VIEW_W - MAP_W - 4;
  int oy = 6;
  int y;
  int x;
  int px = player_x >> 8;
  int py = player_y >> 8;
  for (y = 0; y < MAP_H; y++) {
    for (x = 0; x < MAP_W; x++) {
      int cell = cells[y][x];
      short color = (short)0x111;
      if (cell == CELL_FLOOR || cell == CELL_EXIT) color = (short)0x222;
      else if (cell == CELL_BRICK) color = (short)0x622;
      else if (cell == CELL_METAL) color = (short)0x667;
      else if (cell == CELL_DOOR) color = (short)0xEE4;
      else if (cell == CELL_GATE) color = (short)0xF2F;
      else if (cell == CELL_STONE) color = (short)0x444;
      if (cell == CELL_EXIT) color = (short)0x2FF;
      view[(oy + y) * VIEW_W + ox + x] = color;
    }
  }
  if (px >= 0 && py >= 0 && px < MAP_W && py < MAP_H) {
    view[(oy + py) * VIEW_W + ox + px] = (short)0xFFF;
  }
  for (x = 0; x < actor_count; x++) {
    int ax;
    int ay;
    if (!actors[x].alive) continue;
    ax = actors[x].x >> 8;
    ay = actors[x].y >> 8;
    if (ax < 0 || ay < 0 || ax >= MAP_W || ay >= MAP_H) continue;
    if (actors[x].kind == KIND_SENTRY) view[(oy + ay) * VIEW_W + ox + ax] = (short)0xF22;
    else if (actors[x].kind == KIND_CORE) view[(oy + ay) * VIEW_W + ox + ax] = (short)0xF2F;
    else view[(oy + ay) * VIEW_W + ox + ax] = (short)0x2F2;
  }
}

/* A short bolt in the center of the view for the frames after a shot. */
static void draw_flash(void) {
  int y;
  if (shot_flash <= 0) return;
  for (y = VIEW_H / 2 - 8; y <= VIEW_H / 2 + 4; y++) {
    if (y < 0 || y >= VIEW_H) continue;
    view[y * VIEW_W + VIEW_CENTER] = (short)0xFF8;
    view[y * VIEW_W + VIEW_CENTER - 1] = (short)0xCC4;
  }
}

/* Write view pixels that differ from the last flush. Each store locks the
 * emulator's pixel buffer, so unchanged columns stay in RAM. */
static void flush_view(void) {
  int y;
  int x;
  for (y = 0; y < VIEW_H; y++) {
    for (x = 0; x < VIEW_W; x++) {
      int si = y * VIEW_W + x;
      short color = view[si];
      if (hurt_timer > 0) {
        int r = (color >> 8) & 15;
        r += 5;
        if (r > 15) r = 15;
        color = (short)((color & 0x0FF) | (r << 8));
      }
      if (color != shadow[si]) {
        pixels[y * PIXEL_STRIDE + x] = color;
        shadow[si] = color;
      }
    }
  }
}

/* Append a string into a HUD line. */
static void hud_text(char* buf, int* n, char* text) {
  while (*text != 0 && *n < 79) {
    buf[*n] = *text;
    *n = *n + 1;
    text++;
  }
}

/* Append a non-negative integer. */
static void hud_int(char* buf, int* n, int value) {
  char tmp[8];
  int k = 0;
  if (value < 0) value = 0;
  if (value == 0) {
    tmp[k++] = '0';
  }
  while (value > 0 && k < 8) {
    tmp[k++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (k > 0 && *n < 79) {
    k--;
    buf[*n] = tmp[k];
    *n = *n + 1;
  }
}

/* Write one tile row. Spaces and the rest of the row stay transparent. */
static void draw_row(int row, char* text, int color) {
  int x;
  int n = 0;
  while (text[n] != 0 && n < TILE_ROW_WIDTH) n++;
  for (x = 0; x < TILE_ROW_WIDTH; x++) {
    if (x < n && text[x] != ' ') {
      tiles[row * TILE_ROW_WIDTH + x] = (short)((color << 8) | text[x]);
    } else {
      tiles[row * TILE_ROW_WIDTH + x] = 255;
    }
  }
}

/* Center one line. The caller passes a single string; spaces stay clear. */
static void draw_centered(int row, char* text, int color) {
  char line[80];
  int n = 0;
  int i = 0;
  int pad;
  while (text[n] != 0 && n < 80) n++;
  pad = (80 - n) / 2;
  while (i < pad && i < 79) {
    line[i] = ' ';
    i++;
  }
  n = 0;
  while (text[n] != 0 && i < 79) {
    line[i] = text[n];
    i++;
    n++;
  }
  line[i] = 0;
  draw_row(row, line, color);
}

/* Title instructions, or a short status line once the vault is open. */
static void present_hud(int state) {
  char line[80];
  int n;
  int x;

  if (lantern_bench) return;
  if (hud_mode != state) {
    for (x = 0; x < TILE_ROW_WIDTH * TILE_COL_HEIGHT; x++) tiles[x] = 255;
    hud_mode = state;
    if (state == STATE_TITLE) {
      draw_centered(2, "LANTERN", 0xFF);
      draw_centered(4, "Grok 4.7", 0xFC);
      draw_centered(8, "Destroy the three purple cores.", 0xE3);
      draw_centered(10, "Then reach the cyan crystal.", 0x1F);
      draw_centered(14, "WASD moves. Arrows or mouse look.", 0xFF);
      draw_centered(16, "Click or F shoots. E opens.", 0xFF);
      draw_centered(18, "Right mouse walks. Q quits.", 0xFF);
      draw_centered(52, "SPACE or click to begin.", 0xFC);
    }
  }
  if (state == STATE_TITLE) return;

  if (state == STATE_DEAD) {
    draw_centered(2, "Dead. Press R.", 0xE0);
    return;
  }
  if (state == STATE_WON) {
    draw_centered(2, "Escaped. Press R.", 0x1F);
    return;
  }
  if (state == STATE_PAUSE) {
    draw_centered(2, "Paused. Press P.", 0xFF);
    return;
  }

  n = 0;
  hud_text(line, &n, "HP ");
  hud_int(line, &n, player_hp);
  if (cores_left > 0) {
    hud_text(line, &n, "   ");
    hud_int(line, &n, cores_left);
    hud_text(line, &n, " cores");
  }
  line[n] = 0;
  draw_row(0, line, hurt_timer > 0 ? 0xE0 : 0xFF);
  if (lantern_time < message_until && message[0] != 0) {
    draw_row(1, message, 0xFF);
  } else {
    draw_row(1, "", 0);
  }
}

/* Flicker only the lantern flame. Repainting the whole sprite would be a thousand MMIO stores. */
static void pulse_lantern(int glow) {
  int y;
  int x;
  short flame = (glow & 1) ? (short)0xFF6 : (short)0xEC2;
  volatile short* dst = sprites + SPRITE_LANTERN * 1024;
  for (y = 10; y <= 20; y++) {
    for (x = 13; x <= 18; x++) {
      dst[y * 32 + x] = flame;
    }
  }
}

/* Bob the lantern and show the muzzle flash while a shot is on screen. */
static void place_hardware_sprites(void) {
  int bob = 0;
  if (player_moving) {
    int phase = (int)((lantern_time >> 2) & 7);
    bob = phase < 4 ? phase * 2 : (8 - phase) * 2;
  }
  pulse_lantern((int)(lantern_time >> 2));
  set_sprite_coords(SPRITE_LANTERN, 280, (unsigned)(200 + bob / 2));
  if (shot_flash > 0) set_sprite_coords(SPRITE_MUZZLE, 248, 188);
  else set_sprite_coords(SPRITE_MUZZLE, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
}

int render_init(volatile short* pixel_fb, volatile short* tile_fb, volatile short* sprite_fb) {
  int i;
  pixels = pixel_fb;
  tiles = tile_fb;
  sprites = sprite_fb;
  wallbuf = (short*)malloc(VIEW_W * VIEW_H * 2);
  view = (short*)malloc(VIEW_W * VIEW_H * 2);
  shadow = (short*)malloc(VIEW_W * VIEW_H * 2);
  if (wallbuf == 0 || view == 0 || shadow == 0) {
    return -1;
  }
  for (i = 0; i < VIEW_W * VIEW_H; i++) shadow[i] = (short)0xFFFF;
  init_trig();
  init_textures();
  init_gradients();
  paint_rows(KIND_SENTRY, sentry_rows);
  paint_rows(KIND_CORE, core_rows);
  paint_rows(KIND_HEALTH, health_rows);
  paint_rows(KIND_CRYSTAL, crystal_rows);
  clear_sprite(SPRITE_MUZZLE);
  clear_sprite(SPRITE_CROSS);
  clear_sprite(SPRITE_LANTERN);
  paint_crosshair();
  paint_muzzle();
  paint_lantern_sprite(0);
  set_sprite_scale(SPRITE_MUZZLE, 1);
  set_sprite_scale(SPRITE_CROSS, 1);
  set_sprite_scale(SPRITE_LANTERN, 1);
  /* Scale 1 draws a 64px sprite at twice the stored coordinate (docs/mem_map.md). */
  set_sprite_coords(SPRITE_CROSS, 144, 104);
  set_sprite_coords(SPRITE_LANTERN, 280, 200);
  set_sprite_coords(SPRITE_MUZZLE, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  render_sync_facing();
  have_walls = 0;
  hud_mode = -1;
  return 0;
}

void render_frame(int state, int show_map) {
  audio_pump();
  render_sync_facing();
  rebuild_walls();
  memcpy(view, wallbuf, VIEW_W * VIEW_H * 2);
  draw_sprites();
  draw_flash();
  if (show_map && state != STATE_TITLE) draw_minimap();
  flush_view();
  present_hud(state);
  if (!lantern_bench) place_hardware_sprites();
  if (shot_flash > 0) shot_flash--;
}

void render_hide_sprites(void) {
  if (sprites == 0) return;
  set_sprite_coords(SPRITE_MUZZLE, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  set_sprite_coords(SPRITE_CROSS, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  set_sprite_coords(SPRITE_LANTERN, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
}

unsigned view_checksum(void) {
  unsigned sum = 0;
  int i;
  for (i = 0; i < VIEW_W * VIEW_H; i++) {
    sum += (unsigned short)view[i];
  }
  return sum;
}

/* One view pixel as a letter: R red figure, P purple core, G green cross, C crystal. */
static char preview_glyph(int color) {
  int r = (color >> 8) & 15;
  int g = (color >> 4) & 15;
  int b = color & 15;
  int lum = r * 2 + g + b;
  if (r >= 12 && g >= 12 && b >= 12) return 'W';
  if (r >= 9 && r > g + 3 && r > b + 2) return 'R';
  if (g >= 9 && g > r + 3 && g > b + 2) return 'G';
  if (b >= 8 && g >= 8 && b + g > r * 2 + 4) return 'C';
  if (r >= 8 && b >= 8 && r + b > g * 2 + 4) return 'P';
  if (lum >= 36) return '#';
  if (lum >= 22) return 'O';
  if (lum >= 12) return ':';
  if (lum >= 5) return '.';
  return ' ';
}

/* Print `w` by `h` view pixels starting at (x0, y0), one character per pixel. */
void view_preview_box(int x0, int y0, int w, int h) {
  int y;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x0 + w > VIEW_W) w = VIEW_W - x0;
  if (y0 + h > VIEW_H) h = VIEW_H - y0;
  for (y = y0; y < y0 + h; y++) {
    char line[96];
    int x;
    int n = 0;
    for (x = x0; x < x0 + w; x++) {
      line[n++] = preview_glyph(view[y * VIEW_W + x] & 0xFFF);
    }
    line[n++] = '\n';
    line[n] = 0;
    puts(line);
  }
}

/* Print every pixel of the composed view. */
void view_preview(void) {
  view_preview_box(0, 0, VIEW_W, VIEW_H);
}
