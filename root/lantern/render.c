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
      int stone = 0x777;
      int brick = 0x943;
      int metal = 0x89A;
      int door = 0x742;
      int gate = 0x211;
      int n = (x * 17 + y * 13) ^ (x << 3);
      int offset = ((y >> 3) & 1) << 2;
      if ((n & 7) == 0) stone = 0x333;
      else if ((n & 7) == 1) stone = 0x777;
      if ((y & 7) == 0 || ((x + offset) & 7) == 0) brick = 0x321;
      if ((y % 5) == 0) metal = 0x456;
      if ((x & 7) == 2 && (y % 5) == 2) metal = 0xCDE;
      if ((x & 3) == 0) door = 0x321;
      if (y >= 7 && y <= 9 && x >= 3 && x <= 12) door = 0x888;
      if ((x & 3) == 0) gate = 0x538;
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

/* Ceiling is dark blue, brighter overhead. Floor is dark brown, not yellow. */
static void init_gradients(void) {
  int y;
  for (y = 0; y < VIEW_H; y++) {
    if (y < VIEW_H / 2) {
      int d = (VIEW_H / 2 - 1) - y;
      int level = d / 8;
      int b;
      int g;
      if (level > 7) level = 7;
      b = 1 + level;
      g = level / 3;
      ceil_row[y] = (short)((g << 4) | b);
    } else {
      int d = y - VIEW_H / 2;
      int level = d / 8;
      int r;
      int g;
      if (level > 7) level = 7;
      r = 3 + level;
      g = 2 + level / 3;
      floor_row[y] = (short)((r << 8) | (g << 4) | 1);
    }
  }
}

/* Paint a 16x16 billboard. Pixels outside the shape stay transparent. */
static void paint_art(int kind) {
  int y;
  int x;
  short* dst = sprite_art + kind * 256;
  for (y = 0; y < 16; y++) {
    for (x = 0; x < 16; x++) {
      int dx = x - 8;
      int dy = y - 8;
      int d2 = dx * dx + dy * dy;
      short color = (short)0xF000;
      if (kind == KIND_SENTRY) {
        if (d2 < 36) color = (short)0x611;
        if (d2 < 10) color = (short)0xD22;
        if (y > 11 && x > 5 && x < 10) color = (short)0x321;
        if (d2 < 3 && y < 9) color = (short)0xEEE;
      } else if (kind == KIND_CORE) {
        if (d2 < 40) color = (short)0x808;
        if (d2 < 16) color = (short)0xD4D;
        if (d2 < 4) color = (short)0xFEE;
      } else if (kind == KIND_HEALTH) {
        if (d2 < 42) color = (short)0x131;
        if ((x >= 6 && x <= 9) || (y >= 6 && y <= 9)) color = (short)0x2E2;
      } else if (d2 < 48 && (x + y) > 6 && (x + y) < 24 && (y + 8) > x && x + 8 > y) {
        color = (short)0x0DD;
        if (d2 < 8) color = (short)0x8FF;
      }
      dst[y * 16 + x] = color;
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
  int ang = wrap_angle(player_angle + (col - VIEW_CENTER));
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
  rel = col - VIEW_CENTER;
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
  int col;
  if (delta > 512) delta -= ANGLE_FULL;
  if (delta < -512) delta += ANGLE_FULL;
  if (moved || delta <= -VIEW_W || delta >= VIEW_W) {
    for (col = 0; col < VIEW_W; col++) {
      if ((col & 31) == 0) audio_pump();
      cast_column(col);
    }
  } else if (delta != 0) {
    shift_walls(delta);
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
  int fx = lantern_dir_x;
  int fy = lantern_dir_y;
  int rx = lantern_right_x;
  int ry = lantern_right_y;

  for (i = 0; i < actor_count; i++) {
    struct Actor* actor = &actors[i];
    int dx;
    int dy;
    int rel_r;
    int rel_f;
    int wall;
    int sprite_h;
    int scol;
    if (!actor->alive) continue;
    dx = actor->x - player_x;
    dy = actor->y - player_y;
    rel_r = (dx * rx + dy * ry) >> 8;
    rel_f = (dx * fx + dy * fy) >> 8;
    if (rel_f < 24 || rel_f > 5000) continue;
    wall = PROJECTION / rel_f;
    sprite_h = (PROJECTION * 2) / 3 / rel_f;
    if (sprite_h < 2) continue;
    if (sprite_h > 90) sprite_h = 90;
    scol = VIEW_CENTER + (rel_r * 163) / rel_f;
    order[n] = n;
    depthv[n] = rel_f;
    kindv[n] = actor->kind;
    sizev[n] = sprite_h;
    leftv[n] = scol - sprite_h / 2;
    topv[n] = VIEW_H / 2 + wall / 2 - sprite_h;
    hurtv[n] = actor->hurt;
    n++;
  }

  {
    int dx = ((exit_x << 8) + 128) - player_x;
    int dy = ((exit_y << 8) + 128) - player_y;
    int rel_r = (dx * rx + dy * ry) >> 8;
    int rel_f = (dx * fx + dy * fy) >> 8;
    if (rel_f >= 24 && rel_f <= 5000 && n < MAX_ACTORS + 1) {
      int wall = PROJECTION / rel_f;
      int sprite_h = (PROJECTION * 2) / 3 / rel_f;
      if (sprite_h >= 2) {
        if (sprite_h > 90) sprite_h = 90;
        order[n] = n;
        depthv[n] = rel_f;
        kindv[n] = KIND_CRYSTAL;
        sizev[n] = sprite_h;
        leftv[n] = VIEW_CENTER + (rel_r * 163) / rel_f - sprite_h / 2;
        topv[n] = VIEW_H / 2 + wall / 2 - sprite_h;
        hurtv[n] = 0;
        n++;
      }
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

/* A small map in the corner of the pixel view. */
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
      else if (cell == CELL_DOOR) color = (short)0x852;
      else if (cell == CELL_GATE) color = (short)0x537;
      else if (cell == CELL_STONE) color = (short)0x444;
      if (cell == CELL_EXIT) color = (short)0x0CC;
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
    if ((y & 15) == 0) audio_pump();
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

/* Write one tile row. Spaces stay transparent so the picture shows through. */
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

/* Status line, and the title card when the mode changes. */
static void present_hud(int state) {
  char line[80];
  int n = 0;
  int color = hurt_timer > 0 ? 0xE0 : 0xFC;
  int facing = ((player_angle + 128) >> 8) & 3;
  char face = 'E';
  int x;

  if (lantern_bench) return;
  if (hud_mode != state) {
    for (x = 0; x < TILE_ROW_WIDTH * TILE_COL_HEIGHT; x++) tiles[x] = 255;
    hud_mode = state;
    if (state == STATE_TITLE) {
      draw_row(8, "LANTERN", 0xFC);
      draw_row(10, "Three cores seal the vault. Shatter them.", 0xFC);
      draw_row(12, "Then walk through the gate.", 0xFC);
      draw_row(46, "WASD moves. Mouse looks. Hold right button to walk.", 0xB8);
      draw_row(48, "Left button or F fires. E opens. M map. Q quits.", 0xB8);
      draw_row(50, "Press space or click to enter.", 0xFF);
    }
  }
  if (state == STATE_TITLE) return;

  hud_text(line, &n, "HP ");
  hud_int(line, &n, player_hp);
  hud_text(line, &n, "   Cores ");
  hud_int(line, &n, cores_total - cores_left);
  hud_text(line, &n, "/");
  hud_int(line, &n, cores_total);
  hud_text(line, &n, "   ");
  if (facing == 1) face = 'S';
  else if (facing == 2) face = 'W';
  else if (facing == 3) face = 'N';
  line[n++] = face;
  line[n] = 0;
  draw_row(0, line, color);

  if (state == STATE_PAUSE) {
    draw_row(1, "Paused", 0xFF);
  } else if (lantern_time < message_until && message[0] != 0) {
    draw_row(1, message, color);
  } else if (state == STATE_DEAD || state == STATE_WON) {
    draw_row(1, message, color);
  } else {
    draw_row(1, "F fires   E opens   M map   R resets   Q quits", 0xB8);
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
  set_sprite_coords(SPRITE_LANTERN, 590, (unsigned)(428 + bob));
  if (shot_flash > 0) set_sprite_coords(SPRITE_MUZZLE, 540, 400);
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
  paint_art(KIND_SENTRY);
  paint_art(KIND_CORE);
  paint_art(KIND_HEALTH);
  paint_art(KIND_CRYSTAL);
  clear_sprite(SPRITE_MUZZLE);
  clear_sprite(SPRITE_CROSS);
  clear_sprite(SPRITE_LANTERN);
  paint_crosshair();
  paint_muzzle();
  paint_lantern_sprite(0);
  set_sprite_scale(SPRITE_MUZZLE, 0);
  set_sprite_scale(SPRITE_CROSS, 0);
  set_sprite_scale(SPRITE_LANTERN, 0);
  set_sprite_coords(SPRITE_CROSS, 304, 224);
  set_sprite_coords(SPRITE_LANTERN, 590, 428);
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

void view_preview(void) {
  int y;
  for (y = 0; y < VIEW_H; y += 6) {
    char line[42];
    int x;
    int n = 0;
    for (x = 0; x < VIEW_W; x += 4) {
      int color = view[y * VIEW_W + x] & 0xFFF;
      int lum = ((color >> 8) & 15) * 2 + ((color >> 4) & 15) + (color & 15);
      char glyph = ' ';
      if (lum >= 48) glyph = '#';
      else if (lum >= 36) glyph = 'O';
      else if (lum >= 24) glyph = 'o';
      else if (lum >= 14) glyph = ':';
      else if (lum >= 6) glyph = '.';
      line[n++] = glyph;
    }
    line[n++] = '\n';
    line[n] = 0;
    puts(line);
  }
}
