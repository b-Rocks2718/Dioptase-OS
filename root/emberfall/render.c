/* Emberfall renderer: builds art, keeps the arena, draws HUD and menus. */

#include "render.h"

#include "../crt/sys.h"
#include "../crt/vga.h"

#define TRANSPARENT_TILE 128
#define TILE_PIXELS 64

#define BG_W 320
#define BG_H 236
#define BG_CELLS 75520

#define CURSOR_SPRITE 0
#define SPRITE_CELLS 1024

static volatile short* PIXEL;
static volatile short* TFB;
static volatile short* TMAP;
static volatile short* SPRITES;

/* Pre-rendered arena background, copied back over dirty rectangles. */
static short bg[75520];

/* Procedural sprite art, 0xF000 means transparent. */
static short art_player[144];
static short art_slime[144];
static short art_bat[144];
static short art_skeleton[144];
static short art_orc[144];
static short art_wraith[144];
static short art_golem[256];
static short art_boss[576];
static short art_gem[36];
static short art_bolt[16];
static short art_fireball[36];
static short art_shard[16];

/* Last drawn boxes for dirty-rectangle erase. */
static int old_enemy_active[48];
static int old_enemy_x[48];
static int old_enemy_y[48];
static int old_enemy_size[48];
static int old_proj_active[64];
static int old_proj_x[64];
static int old_proj_y[64];
static int old_proj_size[64];
static int old_gem_active[64];
static int old_gem_x[64];
static int old_gem_y[64];
static int old_part_active[96];
static int old_part_x[96];
static int old_part_y[96];
static int old_player_active;
static int old_player_x;
static int old_player_y;

/* Render-only shake offsets, from a private LCG so game rng stays pure. */
static unsigned render_rng = 0xC0FFEEu;
static int shake_x;
static int shake_y;

static unsigned render_rand(void) {
  render_rng ^= render_rng << 13;
  render_rng ^= render_rng >> 17;
  render_rng ^= render_rng << 5;
  return render_rng;
}

/* Write one tile cell. */
static void put_tile(int x, int y, int tile, int color) {
  if (x < 0 || x >= 80 || y < 0 || y >= 60) return;
  TFB[y * 80 + x] = (short)((color << 8) | (tile & 0xFF));
}

/* Draw a NUL-terminated string in tile cells. */
void render_text(int x, int y, char* text, int color) {
  int i = 0;
  while (text[i] != 0) {
    put_tile(x + i, y, text[i], color);
    i++;
  }
}

/* Draw a right-aligned decimal in a fixed width. */
void render_number(int x, int y, int value, int width, int color) {
  int i = 0;
  int div = 1;
  int started = 0;
  int d = 0;
  while (i < width) {
    put_tile(x + i, y, ' ', color);
    i++;
  }
  if (value < 0) value = 0;
  i = 1;
  while (i < width) {
    div *= 10;
    i++;
  }
  i = 0;
  while (i < width) {
    d = value / div;
    value -= d * div;
    div /= 10;
    if (d != 0 || i == width - 1) started = 1;
    if (started) put_tile(x + i, y, '0' + d, color);
    i++;
  }
}

/* Fill a tile row range with the transparent tile. */
static void clear_tile_rows(int y0, int y1) {
  int y = y0;
  while (y <= y1) {
    int x = 0;
    while (x < 80) {
      TFB[y * 80 + x] = (short)((0 << 8) | TRANSPARENT_TILE);
      x++;
    }
    y++;
  }
}

/* Fill every tile cell with transparency. */
void render_clear_tiles(void) {
  clear_tile_rows(0, 59);
}

/* Fill one 8x8 hardware tile with a solid pixel. */
static void fill_hw_tile(int tile, short px) {
  int i = 0;
  while (i < 64) {
    TMAP[tile * 64 + i] = px;
    i++;
  }
}

/* Cursor arrow shape, tip at (0,0) with a short tail. */
static int cursor_inside(int x, int y) {
  if (x < 0 || y < 0 || x >= 32 || y >= 32) return 0;
  if (y < 13 && x * 4 <= y * 3) return 1;
  return y >= 9 && y < 18 && x >= y - 8 && x <= y - 6;
}

/* Paint sprite 0 as a white arrow with a black outline. */
static void init_cursor(void) {
  int y = 0;
  while (y < 32) {
    int x = 0;
    while (x < 32) {
      short px = TRANSPARENT_PX;
      if (cursor_inside(x, y)) {
        int edge = !cursor_inside(x - 1, y) || !cursor_inside(x + 1, y) ||
          !cursor_inside(x, y - 1) || !cursor_inside(x, y + 1);
        px = edge ? 0x000 : 0xFFF;
      }
      SPRITES[y * 32 + x] = px;
      x++;
    }
    y++;
  }
  set_sprite_scale(CURSOR_SPRITE, 0);
}

/* Place the menu cursor sprite at screen pixels. */
void render_cursor(int x, int y) {
  set_sprite_coords(CURSOR_SPRITE, x, y);
}

/* Park the cursor sprite off screen. */
void render_hide_cursor(void) {
  set_sprite_coords(CURSOR_SPRITE, 1000, 1000);
}

/* Fill an art buffer with transparency. */
static void art_clear(short* art, int cells) {
  int i = 0;
  while (i < cells) {
    art[i] = TRANSPARENT_PX;
    i++;
  }
}

/* Plot one art pixel when inside the square. */
static void art_px(short* art, int side, int x, int y, short color) {
  if (x < 0 || y < 0 || x >= side || y >= side) return;
  art[y * side + x] = color;
}

/* Filled circle in an art buffer. */
static void art_circle(short* art, int side, int cx, int cy, int r,
    short color) {
  int y = cy - r;
  while (y <= cy + r) {
    int x = cx - r;
    while (x <= cx + r) {
      int dx = x - cx;
      int dy = y - cy;
      if (dx * dx + dy * dy <= r * r) art_px(art, side, x, y, color);
      x++;
    }
    y++;
  }
}

/* Two eyes: white with dark pupils, facing down the screen. */
static void art_eyes(short* art, int side, int y, int lx, int rx) {
  art_px(art, side, lx, y, 0xEEE);
  art_px(art, side, lx + 1, y, 0xEEE);
  art_px(art, side, rx, y, 0xEEE);
  art_px(art, side, rx + 1, y, 0xEEE);
  art_px(art, side, lx, y + 1, 0x111);
  art_px(art, side, rx + 1, y + 1, 0x111);
}

/* Build all sprite art procedurally so no binary blobs ship. */
static void init_art(void) {
  /* Warden: deep blue robe, warm face, ember staff. */
  art_clear(art_player, 144);
  art_circle(art_player, 12, 6, 7, 5, 0x410);
  art_circle(art_player, 12, 6, 7, 4, 0xA32);
  art_circle(art_player, 12, 6, 6, 3, 0xF9B);
  art_eyes(art_player, 12, 5, 4, 6);
  art_px(art_player, 12, 9, 2, 0x00F);
  art_px(art_player, 12, 9, 3, 0xF80);
  art_px(art_player, 12, 9, 4, 0xF80);
  art_px(art_player, 12, 9, 5, 0x641);
  art_px(art_player, 12, 9, 6, 0x641);
  art_px(art_player, 12, 9, 7, 0x641);
  art_px(art_player, 12, 9, 8, 0x641);
  /* Slime: green blob with highlight. */
  art_clear(art_slime, 144);
  art_circle(art_slime, 12, 6, 7, 5, 0x1A1);
  art_circle(art_slime, 12, 6, 7, 4, 0x3C3);
  art_circle(art_slime, 12, 4, 5, 2, 0x7F6);
  art_eyes(art_slime, 12, 6, 4, 6);
  /* Bat: purple body with wide wings. */
  art_clear(art_bat, 144);
  {
    int x = 0;
    while (x < 12) {
      int wing = x < 6 ? x : 11 - x;
      art_px(art_bat, 12, x, 4 + (5 - wing) / 2, 0xA3C);
      art_px(art_bat, 12, x, 5 + (5 - wing) / 2, 0xA3C);
      x++;
    }
  }
  art_circle(art_bat, 12, 6, 6, 4, 0x516);
  art_circle(art_bat, 12, 6, 6, 3, 0xC2E);
  art_eyes(art_bat, 12, 5, 5, 6);
  /* Skeleton: pale skull with dark sockets. */
  art_clear(art_skeleton, 144);
  art_circle(art_skeleton, 12, 6, 6, 5, 0x888);
  art_circle(art_skeleton, 12, 6, 6, 4, 0xDDD);
  art_circle(art_skeleton, 12, 4, 5, 1, 0x111);
  art_circle(art_skeleton, 12, 8, 5, 1, 0x111);
  art_px(art_skeleton, 12, 5, 8, 0x111);
  art_px(art_skeleton, 12, 6, 8, 0x111);
  art_px(art_skeleton, 12, 7, 8, 0x111);
  /* Orc: moss skin with tusks. */
  art_clear(art_orc, 144);
  art_circle(art_orc, 12, 6, 6, 5, 0x241);
  art_circle(art_orc, 12, 6, 6, 4, 0x4A2);
  art_eyes(art_orc, 12, 5, 4, 6);
  art_px(art_orc, 12, 4, 8, 0xFFF);
  art_px(art_orc, 12, 7, 8, 0xFFF);
  art_circle(art_orc, 12, 6, 3, 2, 0x241);
  /* Wraith: cyan shroud fading downward. */
  art_clear(art_wraith, 144);
  art_circle(art_wraith, 12, 6, 5, 5, 0x396);
  art_circle(art_wraith, 12, 6, 5, 4, 0x7BE);
  {
    int x = 3;
    while (x < 9) {
      art_px(art_wraith, 12, x, 8, 0x7BE);
      if ((x & 1) == 0) art_px(art_wraith, 12, x, 9, 0x7BE);
      x++;
    }
  }
  art_eyes(art_wraith, 12, 4, 4, 6);
  /* Golem: gray rock with ember cracks. */
  art_clear(art_golem, 256);
  art_circle(art_golem, 16, 8, 8, 7, 0x555);
  art_circle(art_golem, 16, 8, 8, 6, 0x888);
  art_circle(art_golem, 16, 6, 6, 2, 0xAAA);
  art_px(art_golem, 16, 7, 8, 0xF40);
  art_px(art_golem, 16, 8, 9, 0xF40);
  art_px(art_golem, 16, 9, 8, 0xF40);
  art_eyes(art_golem, 16, 6, 5, 9);
  /* Boss: horned shadow with burning eyes. */
  art_clear(art_boss, 576);
  art_circle(art_boss, 24, 12, 12, 10, 0x208);
  art_circle(art_boss, 24, 12, 12, 9, 0x419);
  art_circle(art_boss, 24, 12, 12, 7, 0x61B);
  art_px(art_boss, 24, 4, 4, 0xDDD);
  art_px(art_boss, 24, 5, 4, 0xDDD);
  art_px(art_boss, 24, 4, 5, 0xDDD);
  art_px(art_boss, 24, 5, 5, 0xDDD);
  art_px(art_boss, 24, 19, 4, 0xDDD);
  art_px(art_boss, 24, 18, 4, 0xDDD);
  art_px(art_boss, 24, 19, 5, 0xDDD);
  art_px(art_boss, 24, 18, 5, 0xDDD);
  art_circle(art_boss, 24, 9, 10, 2, 0xF00);
  art_circle(art_boss, 24, 15, 10, 2, 0xF00);
  art_px(art_boss, 24, 10, 15, 0xF80);
  art_px(art_boss, 24, 11, 15, 0xF80);
  art_px(art_boss, 24, 12, 15, 0xF80);
  art_px(art_boss, 24, 13, 15, 0xF80);
  /* Gem: blue diamond with white spark. */
  art_clear(art_gem, 36);
  art_px(art_gem, 6, 2, 0, 0x5BF);
  art_px(art_gem, 6, 3, 0, 0x5BF);
  art_px(art_gem, 6, 1, 1, 0x5BF);
  art_px(art_gem, 6, 2, 1, 0xFFF);
  art_px(art_gem, 6, 3, 1, 0x5BF);
  art_px(art_gem, 6, 4, 1, 0x5BF);
  art_px(art_gem, 6, 1, 2, 0x5BF);
  art_px(art_gem, 6, 2, 2, 0x3AF);
  art_px(art_gem, 6, 3, 2, 0x3AF);
  art_px(art_gem, 6, 4, 2, 0x5BF);
  art_px(art_gem, 6, 2, 3, 0x5BF);
  art_px(art_gem, 6, 3, 3, 0x5BF);
  art_px(art_gem, 6, 2, 4, 0x5BF);
  art_px(art_gem, 6, 3, 4, 0x5BF);
  /* Bolt: hot orange diamond. */
  art_clear(art_bolt, 16);
  art_px(art_bolt, 4, 1, 0, 0xF80);
  art_px(art_bolt, 4, 2, 0, 0xF80);
  art_px(art_bolt, 4, 0, 1, 0xF80);
  art_px(art_bolt, 4, 1, 1, 0xFF0);
  art_px(art_bolt, 4, 2, 1, 0xFF0);
  art_px(art_bolt, 4, 3, 1, 0xF80);
  art_px(art_bolt, 4, 0, 2, 0xF80);
  art_px(art_bolt, 4, 1, 2, 0xFF0);
  art_px(art_bolt, 4, 2, 2, 0xFF0);
  art_px(art_bolt, 4, 3, 2, 0xF80);
  art_px(art_bolt, 4, 1, 3, 0xF80);
  art_px(art_bolt, 4, 2, 3, 0xF80);
  /* Fireball: red core with yellow rim. */
  art_clear(art_fireball, 36);
  art_circle(art_fireball, 6, 3, 3, 2, 0xFF0);
  art_circle(art_fireball, 6, 3, 3, 1, 0xF00);
  /* Shard: pale ice splinter. */
  art_clear(art_shard, 16);
  art_px(art_shard, 4, 2, 0, 0xBFF);
  art_px(art_shard, 4, 1, 1, 0xBFF);
  art_px(art_shard, 4, 2, 1, 0xFFF);
  art_px(art_shard, 4, 1, 2, 0xBFF);
  art_px(art_shard, 4, 2, 2, 0xFFF);
  art_px(art_shard, 4, 1, 3, 0xBFF);
}

/* Deterministic meadow: moss base, darker speckles, flowers and stones. */
static void init_bg(void) {
  unsigned rng = 0xBE7713u;
  int i = 0;
  while (i < 75520) {
    int x = i % 320;
    int y = i / 320;
    short c = 0x242;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    if ((rng & 7) == 0) c = 0x232;
    else if ((rng & 31) == 0) c = 0x353;
    /* Soft vignette toward the edges. */
    if (x < 12 || x >= 308 || y < 12 || y >= 224) c = 0x221;
    bg[i] = c;
    i++;
  }
  /* Flowers: tiny red, yellow, and white crosses. */
  {
    int f = 0;
    while (f < 60) {
      int x = 0;
      int y = 0;
      short c = 0xF44;
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      x = 16 + (int)(rng % 288);
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      y = 16 + (int)(rng % 204);
      if ((rng & 3) == 0) c = 0xFF0;
      else if ((rng & 3) == 1) c = 0xFFF;
      bg[y * 320 + x] = c;
      if (x > 0) bg[y * 320 + x - 1] = 0x353;
      if (x < 319) bg[y * 320 + x + 1] = 0x353;
      f++;
    }
  }
  /* Stones: small gray clusters. */
  {
    int s = 0;
    while (s < 24) {
      int x = 0;
      int y = 0;
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      x = 16 + (int)(rng % 288);
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      y = 16 + (int)(rng % 204);
      bg[y * 320 + x] = 0x888;
      if (x < 319) bg[y * 320 + x + 1] = 0x777;
      if (y < 235) bg[(y + 1) * 320 + x] = 0x666;
      s++;
    }
  }
  /* Arena walls: a stone ring just outside the 10 px play margin. */
  {
    int x = 0;
    while (x < 320) {
      int y = 0;
      while (y < 6) {
        short c = ((x + y) & 3) == 0 ? 0x999 : 0x777;
        if (y == 5) c = 0x555;
        bg[y * 320 + x] = c;
        bg[(235 - y) * 320 + x] = c;
        y++;
      }
      x++;
    }
    {
      int y = 6;
      while (y < 230) {
        int xx = 0;
        while (xx < 6) {
          short c = ((xx + y) & 3) == 0 ? 0x999 : 0x777;
          if (xx == 5) c = 0x555;
          bg[y * 320 + xx] = c;
          bg[y * 320 + 319 - xx] = c;
          xx++;
        }
        y++;
      }
    }
  }
}

/* Copy the whole meadow into the pixel framebuffer. */
static void blit_bg_full(void) {
  int i = 0;
  while (i < 75520) {
    PIXEL[i] = bg[i];
    i++;
  }
}

/* Restore one background rectangle with row pointers, no multiplies. */
static void blit_bg_rect(int x, int y, int w, int h) {
  int row = 0;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > 320) w = 320 - x;
  if (y + h > 236) h = 236 - y;
  if (w <= 0 || h <= 0) return;
  while (row < h) {
    volatile short* dst = PIXEL + (y + row) * 320 + x;
    short* src = bg + (y + row) * 320 + x;
    int col = 0;
    while (col < w) {
      dst[col] = src[col];
      col++;
    }
    row++;
  }
}

/* Draw square art with hoisted clipping so the inner loop skips bounds. */
static void blit_art(int x, int y, short* art, int side, int flash) {
  int ax0 = 0;
  int ay0 = 0;
  int ax1 = side;
  int ay1 = side;
  int ay = 0;
  short fill = 0xFFF;
  x += shake_x;
  y += shake_y;
  if (x < 0) ax0 = -x;
  if (y < 0) ay0 = -y;
  if (x + ax1 > 320) ax1 = 320 - x;
  if (y + ay1 > 236) ay1 = 236 - y;
  if (ax0 >= ax1 || ay0 >= ay1) return;
  ay = ay0;
  while (ay < ay1) {
    volatile short* dst = PIXEL + (y + ay) * 320 + x + ax0;
    short* src = art + ay * side + ax0;
    int ax = ax0;
    while (ax < ax1) {
      short px = src[ax - ax0];
      if (px != TRANSPARENT_PX) {
        if (flash) px = fill;
        dst[ax - ax0] = px;
      }
      ax++;
    }
    ay++;
  }
}

/* Draw one arena pixel with shake and clipping. */
static void blit_px(int x, int y, short color) {
  x += shake_x;
  y += shake_y;
  if (x < 0 || x >= 320 || y < 0 || y >= 236) return;
  PIXEL[y * 320 + x] = color;
}

/* Enemy art and size by kind. */
static short* enemy_art(int kind, int* size_out) {
  if (kind == ENEMY_SLIME) {
    *size_out = 12;
    return art_slime;
  }
  if (kind == ENEMY_BAT) {
    *size_out = 12;
    return art_bat;
  }
  if (kind == ENEMY_SKELETON) {
    *size_out = 12;
    return art_skeleton;
  }
  if (kind == ENEMY_ORC) {
    *size_out = 12;
    return art_orc;
  }
  if (kind == ENEMY_WRAITH) {
    *size_out = 12;
    return art_wraith;
  }
  if (kind == ENEMY_GOLEM) {
    *size_out = 16;
    return art_golem;
  }
  *size_out = 24;
  return art_boss;
}

/* Projectile art and size by spell kind. */
static short* projectile_art(int kind, int* size_out) {
  if (kind == SPELL_FIREBALL) {
    *size_out = 6;
    return art_fireball;
  }
  if (kind == SPELL_FROST) {
    *size_out = 4;
    return art_shard;
  }
  *size_out = 4;
  return art_bolt;
}

/* Forget all dirty boxes so the next frame redraws everything. */
static void clear_old(void) {
  int i = 0;
  while (i < 48) {
    old_enemy_active[i] = 0;
    i++;
  }
  i = 0;
  while (i < 64) {
    old_proj_active[i] = 0;
    old_gem_active[i] = 0;
    i++;
  }
  i = 0;
  while (i < 96) {
    old_part_active[i] = 0;
    i++;
  }
  old_player_active = 0;
}

/* Map displays, build art and meadow, clear for a run. */
void render_init(void) {
  int i = 0;
  PIXEL = get_pixel_fb();
  TFB = get_tile_fb();
  TMAP = get_tilemap();
  SPRITES = get_spritemap();
  set_pixel_scale(0);
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  load_text_tiles();
  fill_hw_tile(TRANSPARENT_TILE, TRANSPARENT_PX);
  while (i < 16) {
    set_sprite_scale(i, 0);
    set_sprite_coords(i, 1000, 1000);
    i++;
  }
  init_cursor();
  render_hide_cursor();
  init_art();
  init_bg();
  blit_bg_full();
  clear_old();
  render_clear_tiles();
}

/* Repaint the meadow and forget dirty boxes after a menu overlay. */
void render_restore_arena(void) {
  blit_bg_full();
  clear_old();
  render_clear_tiles();
}

/* Erase every box drawn last frame. */
static void erase_old(void) {
  int i = 0;
  while (i < 48) {
    if (old_enemy_active[i]) {
      blit_bg_rect(old_enemy_x[i], old_enemy_y[i] - 4, old_enemy_size[i],
        old_enemy_size[i] + 4);
      old_enemy_active[i] = 0;
    }
    i++;
  }
  i = 0;
  while (i < 64) {
    if (old_proj_active[i]) {
      blit_bg_rect(old_proj_x[i], old_proj_y[i], old_proj_size[i],
        old_proj_size[i]);
      old_proj_active[i] = 0;
    }
    if (old_gem_active[i]) {
      blit_bg_rect(old_gem_x[i], old_gem_y[i], 6, 6);
      old_gem_active[i] = 0;
    }
    i++;
  }
  i = 0;
  while (i < 96) {
    if (old_part_active[i]) {
      blit_bg_rect(old_part_x[i] - 1, old_part_y[i] - 1, 4, 4);
      old_part_active[i] = 0;
    }
    i++;
  }
  if (old_player_active) {
    blit_bg_rect(old_player_x, old_player_y, 12, 12);
    old_player_active = 0;
  }
}

/* Draw a small HP bar above an enemy in arena pixels. */
static void draw_enemy_bar(int x, int y, int size, int hp, int max_hp) {
  int w = size;
  int fill = 0;
  int i = 0;
  if (max_hp <= 0) return;
  if (hp >= max_hp) return;
  fill = w * hp / max_hp;
  while (i < w) {
    short c = i < fill ? 0x0F0 : 0x400;
    blit_px(x + i, y - 3, c);
    blit_px(x + i, y - 2, c);
    i++;
  }
}

/* Draw gems, enemies, shots, warden, and sparks in painter order. */
static void draw_entities(struct Game* game) {
  int i = 0;
  while (i < 64) {
    if (game->gems[i].active) {
      int x = (game->gems[i].x >> 8) - 3;
      int y = (game->gems[i].y >> 8) - 3;
      int bob = (game->tick + i * 13) % 30 < 15 ? 0 : 1;
      blit_art(x, y + bob, art_gem, 6, 0);
      old_gem_active[i] = 1;
      old_gem_x[i] = x;
      old_gem_y[i] = y + bob;
    }
    i++;
  }
  i = 0;
  while (i < 48) {
    if (game->enemies[i].active) {
      int size = 12;
      short* art = enemy_art(game->enemies[i].kind, &size);
      int x = (game->enemies[i].x >> 8) - size / 2;
      int y = (game->enemies[i].y >> 8) - size / 2;
      int flash = game->enemies[i].hit_timer > 0;
      blit_art(x, y, art, size, flash);
      draw_enemy_bar(x, y, size, game->enemies[i].hp,
        game->enemies[i].max_hp);
      old_enemy_active[i] = 1;
      old_enemy_x[i] = x;
      old_enemy_y[i] = y;
      old_enemy_size[i] = size;
    }
    i++;
  }
  i = 0;
  while (i < 64) {
    if (game->projectiles[i].active) {
      int size = 4;
      short* art = projectile_art(game->projectiles[i].kind, &size);
      int x = (game->projectiles[i].x >> 8) - size / 2;
      int y = (game->projectiles[i].y >> 8) - size / 2;
      blit_art(x, y, art, size, 0);
      old_proj_active[i] = 1;
      old_proj_x[i] = x;
      old_proj_y[i] = y;
      old_proj_size[i] = size;
    }
    i++;
  }
  /* Blink the warden while invulnerable. */
  if (game->phase != PHASE_GAMEOVER) {
    if (game->player.invuln_timer == 0 || (game->tick & 1) == 0) {
      int x = (game->player.x >> 8) - 6;
      int y = (game->player.y >> 8) - 6;
      blit_art(x, y, art_player, 12, 0);
      old_player_active = 1;
      old_player_x = x;
      old_player_y = y;
    }
  }
  i = 0;
  while (i < 96) {
    if (game->particles[i].active) {
      int x = game->particles[i].x >> 8;
      int y = game->particles[i].y >> 8;
      /* Young sparks draw 2x2 and shrink to one pixel as they fade. */
      if (game->particles[i].life * 2 > game->particles[i].max_life) {
        blit_px(x, y, game->particles[i].color);
        blit_px(x + 1, y, game->particles[i].color);
        blit_px(x, y + 1, game->particles[i].color);
        blit_px(x + 1, y + 1, game->particles[i].color);
      } else {
        blit_px(x, y, game->particles[i].color);
      }
      old_part_active[i] = 1;
      old_part_x[i] = x;
      old_part_y[i] = y;
    }
    i++;
  }
}

/* Draw a text bar of filled '#' and empty '-' cells. */
static void draw_bar(int x, int y, int w, int filled, int color) {
  int i = 0;
  while (i < w) {
    put_tile(x + i, y, i < filled ? '#' : '-', color);
    i++;
  }
}

/* Two-row HUD plus boss bar, streak, and a bottom hints line. */
static void draw_hud(struct Game* game, int muted) {
  int sec = game_seconds(game);
  int mm = sec / 60;
  int ss = sec - mm * 60;
  int hp_fill = 0;
  int xp_fill = 0;
  int hp_color = 0x1C;
  int low_hp = 0;
  if (game->player.max_hp > 0) {
    hp_fill = 20 * game->player.hp / game->player.max_hp;
    low_hp = game->player.hp * 10 < game->player.max_hp * 3;
  }
  if (game->player.xp_next > 0) {
    xp_fill = 20 * game->player.xp / game->player.xp_next;
  }
  /* Blink the HP bar red when below 30 percent. */
  if (low_hp && (game->tick & 8) != 0) hp_color = 0xE9;
  else if (low_hp) hp_color = 0x1C;
  render_text(1, 0, "HP", 0xFF);
  render_number(4, 0, game->player.hp, 3, low_hp ? 0xE9 : 0xFF);
  render_text(7, 0, "/", 0x92);
  render_number(8, 0, game->player.max_hp, 3, 0x92);
  draw_bar(12, 0, 20, hp_fill, hp_color);
  render_text(33, 0, "LV", 0xFF);
  render_number(35, 0, game->player.level, 2, 0xFD);
  render_text(38, 0, "TIME", 0xFF);
  render_number(43, 0, mm, 2, 0xFF);
  render_text(45, 0, ":", 0xFF);
  if (ss < 10) render_text(46, 0, "0", 0xFF);
  render_number(47, 0, ss, ss < 10 ? 1 : 2, 0xFF);
  render_text(50, 0, "KILLS", 0xFF);
  render_number(56, 0, game->kills, 4, 0xFF);
  render_text(61, 0, "SCORE", 0xFF);
  render_number(67, 0, game->score, 6, 0xFC);
  render_text(1, 1, "XP", 0xFF);
  render_number(4, 1, game->player.xp, 3, 0xFF);
  render_text(7, 1, "/", 0x92);
  render_number(8, 1, game->player.xp_next, 3, 0x92);
  draw_bar(12, 1, 20, xp_fill, 0x5F);
  render_text(33, 1, "BEST", 0xFF);
  render_number(38, 1, game->best, 6, 0xFD);
  if (muted) render_text(50, 1, "[M]UTED", 0xE9);
  else render_text(50, 1, "[M]usic on", 0x92);
  /* Recent chains show their multiplier at the right of row one. */
  if (game->streak >= 3 && game->tick - game->last_kill_tick <= 45) {
    render_text(63, 1, "STREAK x", 0xFC);
    render_number(72, 1, game->streak, 2, 0xFC);
  } else {
    render_text(63, 1, "          ", 0xFF);
  }
  /* A live boss earns a wide bar on row two. */
  {
    int b = 0;
    int boss_hp = -1;
    int boss_max = 1;
    while (b < 48) {
      if (game->enemies[b].active &&
          game->enemies[b].kind == ENEMY_BOSS) {
        boss_hp = game->enemies[b].hp;
        boss_max = game->enemies[b].max_hp;
        break;
      }
      b++;
    }
    if (boss_hp >= 0) {
      int fill = 0;
      if (boss_max > 0) fill = 30 * boss_hp / boss_max;
      render_text(1, 2, "BOSS", 0xE9);
      draw_bar(6, 2, 30, fill, 0xE9);
      render_number(37, 2, boss_hp, 4, 0xFF);
    } else {
      int c = 1;
      while (c < 42) {
        put_tile(c, 2, TRANSPARENT_TILE, 0);
        c++;
      }
    }
  }
  render_text(1, 59, "WASD/arrows move  P pause  M music  Q quit", 0x92);
}

/* Erase dirty boxes, draw entities, refresh the HUD. */
void render_frame(struct Game* game, int muted) {
  if (game->shake_timer > 0) {
    shake_x = (int)(render_rand() % 5) - 2;
    shake_y = (int)(render_rand() % 5) - 2;
  } else {
    shake_x = 0;
    shake_y = 0;
  }
  erase_old();
  draw_entities(game);
  draw_hud(game, muted);
}

/* Fill the arena pixels with one dark color for menus. */
static void darken_arena(void) {
  int i = 0;
  erase_old();
  while (i < 75520) {
    PIXEL[i] = 0x111;
    i++;
  }
}

/* Centered text helper. */
static void center_text(int y, char* text, int color) {
  int len = 0;
  while (text[len] != 0) len++;
  render_text((80 - len) / 2, y, text, color);
}

/* Draw a thin box from '+' '-' and '|' characters. */
static void draw_box(int x, int y, int w, int h, int color) {
  int i = 0;
  put_tile(x, y, '+', color);
  put_tile(x + w - 1, y, '+', color);
  put_tile(x, y + h - 1, '+', color);
  put_tile(x + w - 1, y + h - 1, '+', color);
  i = 1;
  while (i < w - 1) {
    put_tile(x + i, y, '-', color);
    put_tile(x + i, y + h - 1, '-', color);
    i++;
  }
  i = 1;
  while (i < h - 1) {
    put_tile(x, y + i, '|', color);
    put_tile(x + w - 1, y + i, '|', color);
    i++;
  }
}

/* Darken the arena and draw the title screen. */
void render_title(struct Game* game, struct HighScores* hs) {
  int i = 0;
  darken_arena();
  render_clear_tiles();
  center_text(6, "E M B E R F A L L", 0xFC);
  center_text(8, "Warden of the Rift", 0xF9);
  center_text(10, "Survive the horde. Level up. How long can you last?", 0x92);
  center_text(12, "Made by muse-spark-1.3", 0x7B);
  draw_box(24, 16, 32, 12, 0xFC);
  center_text(17, "-- BEST WARDENS --", 0xFD);
  while (i < 5) {
    int mm = hs->time_sec[i] / 60;
    int ss = hs->time_sec[i] - mm * 60;
    render_number(27, 19 + i, hs->score[i], 6, 0xFF);
    render_number(34, 19 + i, mm, 2, 0x92);
    render_text(36, 19 + i, ":", 0x92);
    if (ss < 10) render_text(37, 19 + i, "0", 0x92);
    render_number(38, 19 + i, ss, ss < 10 ? 1 : 2, 0x92);
    render_number(41, 19 + i, hs->kills[i], 4, 0x92);
    render_text(46, 19 + i, "Lv", 0x49);
    render_number(48, 19 + i, hs->level[i], 2, 0x92);
    i++;
  }
  render_text(28, 30, "WASD/arrows: move", 0x92);
  render_text(28, 31, "Spells fire on their own", 0x92);
  render_text(28, 32, "Grab gems, pick upgrades", 0x92);
  render_text(28, 33, "P pause  M music  Q quit", 0x92);
  draw_box(28, 44, 24, 3, 0x1C);
  center_text(45, "[ Enter ] Start", 0xFF);
  draw_box(28, 48, 24, 3, 0x49);
  center_text(49, "[ H ] How to play", 0xFF);
  center_text(54, "Click buttons or press keys. Ctrl-C always quits.", 0x49);
  if (game->best > 0) {
    render_text(28, 14, "Best:", 0xFD);
    render_number(34, 14, game->best, 6, 0xFD);
  }
}

/* Darken the arena and draw the how-to screen. */
void render_help(void) {
  darken_arena();
  render_clear_tiles();
  center_text(4, "HOW TO PLAY", 0xFC);
  render_text(10, 8, "Move with WASD or arrow keys (hold to run).", 0xFF);
  render_text(10, 10, "Your spells fire by themselves at nearby foes.", 0xFF);
  render_text(10, 12, "Pick up blue gems to level up, then choose 1 of 3", 0xFF);
  render_text(10, 13, "upgrades with keys 1/2/3 or the mouse.", 0xFF);
  render_text(10, 15, "Spells:", 0xFD);
  render_text(12, 16, "Ember Bolt: fast bolts. Fireball: big splash.", 0x92);
  render_text(12, 17, "Frost Shard: slows foes. Arc: chain zap.", 0x92);
  render_text(12, 18, "Holy Nova: ring blast around you.", 0x92);
  render_text(10, 20, "Passives:", 0xFD);
  render_text(12, 21, "Boots speed, Rune damage, Heart HP+regen,", 0x92);
  render_text(12, 22, "Magnet pickup range, Tome XP gain.", 0x92);
  render_text(10, 24, "Foes:", 0xFD);
  render_text(12, 25, "Slimes, bats, skeletons, orcs, wraiths, golems.", 0x92);
  render_text(12, 26, "A boss stalks you every 90 seconds.", 0x92);
  render_text(10, 28, "Keys:", 0xFD);
  render_text(12, 29, "P/Esc pause  M music  R restart (pause/over)", 0x92);
  render_text(12, 30, "Q quit  1/2/3 upgrade  Mouse clicks menus", 0x92);
  center_text(36, "Tip: keep moving in wide circles. Do not corner!", 0x1C);
  center_text(44, "[ Press any key or click to go back ]", 0xFF);
  center_text(48, "Emberfall by muse-spark-1.3", 0x7B);
}

/* Darken the arena and draw the pause menu. */
void render_pause(void) {
  darken_arena();
  render_clear_tiles();
  center_text(20, "P A U S E D", 0xFD);
  center_text(22, "The horde waits for no warden... except now.", 0x92);
  draw_box(30, 30, 20, 3, 0x1C);
  center_text(31, "[ P ] Resume", 0xFF);
  draw_box(30, 34, 20, 3, 0x49);
  center_text(35, "[ R ] Restart", 0xFF);
  draw_box(30, 38, 20, 3, 0xE9);
  center_text(39, "[ Q ] Quit", 0xFF);
}

/* Darken the arena and draw three upgrade cards. */
void render_levelup(struct Game* game) {
  int i = 0;
  darken_arena();
  render_clear_tiles();
  center_text(12, "LEVEL UP! Choose one (1/2/3 or click)", 0x1C);
  while (i < 3) {
    int x = 8 + i * 22;
    int kind = game->choices[i].kind;
    int lvl = game->choices[i].level;
    char* title = upgrade_title(kind);
    char* desc = upgrade_desc(kind, lvl);
    draw_box(x, 22, 20, 16, 0xFC);
    render_text(x + 2, 24, title, 0xFF);
    if (lvl > 0) {
      render_text(x + 2, 26, "Lv", 0xFD);
      render_number(x + 5, 26, lvl, 1, 0xFD);
    } else {
      render_text(x + 2, 26, "Bonus", 0xFD);
    }
    /* Wrap the one-line description into the 16-wide card. */
    {
      int col = 0;
      int row = 0;
      int p = 0;
      while (desc[p] != 0 && row < 6) {
        if (desc[p] == ' ' && col >= 12) {
          row++;
          col = 0;
          p++;
          continue;
        }
        put_tile(x + 2 + col, 28 + row, desc[p], 0x92);
        col++;
        p++;
        if (col >= 16) {
          col = 0;
          row++;
        }
      }
    }
    render_text(x + 2, 35, "Press", 0x49);
    render_number(x + 8, 35, i + 1, 1, 0xFF);
    i++;
  }
  render_text(8, 42, "Level:", 0xFF);
  render_number(15, 42, game->player.level, 2, 0xFD);
  render_text(25, 42, "Kills:", 0xFF);
  render_number(32, 42, game->kills, 4, 0xFF);
  render_text(45, 42, "Score:", 0xFF);
  render_number(52, 42, game->score, 6, 0xFC);
}

/* Darken the arena and draw run stats plus ranks. */
void render_gameover(struct Game* game, struct HighScores* hs, int new_best) {
  int i = 0;
  int sec = game_seconds(game);
  int mm = sec / 60;
  int ss = sec - mm * 60;
  darken_arena();
  render_clear_tiles();
  center_text(8, "THE RIFT CLAIMS YOU", 0xE9);
  if (new_best) center_text(10, "NEW BEST! The songs will remember.", 0xFD);
  else center_text(10, "The embers fade... for now.", 0x92);
  render_text(28, 14, "Time:", 0xFF);
  render_number(34, 14, mm, 2, 0xFF);
  render_text(36, 14, ":", 0xFF);
  if (ss < 10) render_text(37, 14, "0", 0xFF);
  render_number(38, 14, ss, ss < 10 ? 1 : 2, 0xFF);
  render_text(28, 15, "Kills:", 0xFF);
  render_number(34, 15, game->kills, 4, 0xFF);
  render_text(28, 16, "Level:", 0xFF);
  render_number(34, 16, game->player.level, 2, 0xFF);
  render_text(28, 17, "Score:", 0xFF);
  render_number(34, 17, game->score, 6, 0xFC);
  render_text(28, 18, "Best:", 0xFF);
  render_number(34, 18, game->best, 6, 0xFD);
  draw_box(24, 22, 32, 10, 0xFC);
  center_text(23, "-- BEST WARDENS --", 0xFD);
  while (i < 5) {
    int m2 = hs->time_sec[i] / 60;
    int s2 = hs->time_sec[i] - m2 * 60;
    render_number(27, 25 + i, hs->score[i], 6, 0xFF);
    render_number(34, 25 + i, m2, 2, 0x92);
    render_text(36, 25 + i, ":", 0x92);
    if (s2 < 10) render_text(37, 25 + i, "0", 0x92);
    render_number(38, 25 + i, s2, s2 < 10 ? 1 : 2, 0x92);
    render_number(41, 25 + i, hs->kills[i], 4, 0x92);
    render_text(46, 25 + i, "Lv", 0x49);
    render_number(48, 25 + i, hs->level[i], 2, 0x92);
    i++;
  }
  draw_box(28, 44, 24, 3, 0x1C);
  center_text(45, "[ Enter ] Retry", 0xFF);
  draw_box(28, 48, 24, 3, 0x49);
  center_text(49, "[ Q ] Quit", 0xFF);
  center_text(54, "Emberfall by muse-spark-1.3", 0x7B);
}
