#include "skyforge.h"
#ifndef SKYFORGE_HOST_TEST
#include "../crt/sys.h"
#include "../crt/vga.h"
#endif

/*
 * Graphical cockpit instrument panel (rows 192..231 of the pixel framebuffer),
 * live topographic radar, artificial horizon, tile-layer shadowed text, metallic
 * SKYFORGE title banner, and hardware aiming/lock-on sprites.
 */

unsigned g_hud_words[6400];
#define g_hud ((unsigned short*)g_hud_words)

/* Immediate-mode tile framebuffer double-entry buffers (40x29 = 1160 shorts = 580 words). */
static unsigned text_want[580];
static unsigned text_shown[580];

/* Big logo banner tile geometry */
#define LOGO_TILE0 128
#define LOGO_COLS 24
#define LOGO_ROWS 3

/* Metallic emerald-to-cyan-to-gold vertical color ramp for the SKYFORGE logo. */
static unsigned short logo_ramp[24] = {
  0x0FF8, 0x0EF6, 0x0DF4, 0x0CF3, 0x0BF2, 0x0AF2, 0x09E2, 0x08D2,
  0x0EDC, 0x0DDA, 0x0CD8, 0x0BD6, 0x0AD5, 0x09C4, 0x08B3, 0x07A2,
  0x06FF, 0x05EF, 0x04DF, 0x03CF, 0x02BF, 0x02AF, 0x019E, 0x018C
};

/* Format signed integer `v` into `buf` (at least 12 bytes) and return `buf`. */
char* fmt_int(char* buf, int v){
  char tmp[12];
  int n = 0;
  bool neg = v < 0;
  unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
  do {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u != 0);
  int i = 0;
  if (neg) buf[i++] = '-';
  while (n > 0) buf[i++] = tmp[--n];
  buf[i] = 0;
  return buf;
}

#ifndef SKYFORGE_HOST_TEST

/* Build the 3x-scaled metallic SKYFORGE logo into tiles LOGO_TILE0..LOGO_TILE0+71. */
static void init_logo_tiles(void){
  char* title = "SKYFORGE";
  unsigned char mask[4608]; /* 192 * 24 pixels */
  for (int i = 0; i < 4608; ++i) mask[i] = 0;

  for (int ch = 0; ch < 8; ++ch){
    int c = title[ch];
    unsigned char* glyph = &font_8x8[(c - 32) * 8];
    int base_x = ch * 24 + 2;
    for (int gy = 0; gy < 8; ++gy){
      int row_bits = glyph[gy];
      for (int gx = 0; gx < 8; ++gx){
        if (!(row_bits & (0x80 >> gx))) continue;
        int px = base_x + gx * 3;
        int py = gy * 3;
        /* Drop shadow and dark outline */
        for (int dy = -1; dy <= 3; ++dy){
          for (int dx = -1; dx <= 3; ++dx){
            int mx = px + dx;
            int my = py + dy;
            if (mx >= 0 && mx < 192 && my >= 0 && my < 24){
              unsigned char val = (dx == 3 || dy == 3) ? 1 : 2;
              if (mask[my * 192 + mx] < val) mask[my * 192 + mx] = val;
            }
          }
        }
        /* Main 3x3 scaled pixel block */
        for (int dy = 0; dy < 3; ++dy){
          for (int dx = 0; dx < 3; ++dx){
            int mx = px + dx;
            int my = py + dy;
            if (mx >= 0 && mx < 192 && my >= 0 && my < 24){
              mask[my * 192 + mx] = 3;
            }
          }
        }
      }
    }
  }

  for (int tr = 0; tr < LOGO_ROWS; ++tr){
    for (int tc = 0; tc < LOGO_COLS; ++tc){
      int tile_id = LOGO_TILE0 + tr * LOGO_COLS + tc;
      volatile short* dst = g_tilemap + tile_id * TILEMAP_SHORTS_PER_TILE;
      for (int y = 0; y < 8; ++y){
        int py = tr * 8 + y;
        for (int x = 0; x < 8; ++x){
          int px = tc * 8 + x;
          unsigned char m = mask[py * 192 + px];
          unsigned short col = TRANSPARENT_12BIT;
          if (m == 1) col = 0x0000;
          else if (m == 2) col = 0x0231;
          else if (m == 3) col = logo_ramp[py];
          dst[y * 8 + x] = (short)col;
        }
      }
    }
  }
}

/* Initialize hardware sprites 0 (aiming reticle) and 1 (target lock box). */
static void init_sprites(void){
  /* Clear all 16 hardware sprites offscreen first */
  for (int s = 0; s < NUM_SPRITES; ++s){
    set_sprite_scale(s, 0);
    set_sprite_coords(s, SPRITE_OFFSCREEN, SPRITE_OFFSCREEN);
  }

  /* Sprite 0: 32x32 Futuristic Starfighter Aiming Reticle */
  volatile short* sp0 = g_spritemap + SPRITE_RETICLE * SPRITE_SHORTS;
  for (int y = 0; y < 32; ++y){
    for (int x = 0; x < 32; ++x){
      unsigned short col = TRANSPARENT_12BIT;
      int dx = x - 16;
      int dy = y - 16;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (dx == 0 && dy == 0){
        col = 0x0FFF; /* Center dot */
      } else if ((dy == 0 && dx >= 4 && dx <= 10) || (dx == 0 && dy >= 4 && dy <= 10)){
        col = 0x0FF2; /* Inner crosshairs */
      } else if ((dx == 12 && dy >= 8 && dy <= 12) || (dy == 12 && dx >= 8 && dx <= 12)){
        col = 0x08F4; /* Corner brackets */
      }
      sp0[y * 32 + x] = (short)col;
    }
  }

  /* Sprite 1: 32x32 Target Lock-On Diamond */
  volatile short* sp1 = g_spritemap + SPRITE_LOCK_BOX * SPRITE_SHORTS;
  for (int y = 0; y < 32; ++y){
    for (int x = 0; x < 32; ++x){
      unsigned short col = TRANSPARENT_12BIT;
      int dx = x - 16;
      int dy = y - 16;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (dx + dy == 14 || dx + dy == 15){
        col = 0x024F; /* Red/orange diamond lock ring */
      } else if ((x == 2 || x == 29) && (y >= 14 && y <= 18)){
        col = 0x04FF;
      }
      sp1[y * 32 + x] = (short)col;
    }
  }
}

/* Initialize font tiles, logo banner tiles, tile framebuffer border, and sprites. */
void hud_init(void){
  set_pixel_scale(0);
  set_tile_scale(TILE_SCALE_HUD);
  set_hscroll(0);
  set_vscroll(0);

  for (int t = 0; t < 128; ++t){
    volatile short* tile = g_tilemap + t * TILEMAP_SHORTS_PER_TILE;
    for (int y = 0; y < 8; ++y){
      int bits = (t >= 32) ? font_8x8[(t - 32) * 8 + y] : 0;
      int above = (t >= 32 && y > 0) ? font_8x8[(t - 32) * 8 + y - 1] : 0;
      for (int x = 0; x < 8; ++x){
        unsigned short c = TRANSPARENT_12BIT;
        if (t == TILE_BLACK){
          c = 0x0000;
        } else if (t == TILE_SOLID){
          c = 0xC000;
        } else if (t == TILE_BORDER){
          c = (y == 0) ? 0x08B4 : 0x0112;
        } else if (t > TILE_BAR_BASE && t < TILE_BAR_BASE + 8){
          c = (x < t - TILE_BAR_BASE) ? 0xC000 : 0x0000;
        } else if (bits & (0x80 >> x)){
          c = 0xC000;
        } else if (x > 0 && (above & (0x80 >> (x - 1)))){
          c = 0x0000; /* Crisp black drop shadow */
        }
        tile[y * 8 + x] = (short)c;
      }
    }
  }

  init_logo_tiles();
  init_sprites();

  for (int i = 0; i < TILE_FB_STRIDE * 60; ++i){
    g_tile_fb[i] = TILE_CLEAR;
  }
  /* Cover pixel rows 232..239 (tile row 29 at scale 1) with an opaque bezel */
  for (int col = 0; col < TEXT_COLS; ++col){
    g_tile_fb[TEXT_BORDER_ROW * TILE_FB_STRIDE + col] = TILE_BORDER;
  }
  for (int i = 0; i < 580; ++i){
    text_want[i] = 0;
    text_shown[i] = 0;
  }
}

/* Clear the desired tile overlay state at the start of a frame. */
void text_begin(void){
  for (int i = 0; i < 580; ++i){
    text_want[i] = 0;
  }
}

/* Set one tile cell (col: 0..39, row: 0..28) in the desired overlay buffer. */
static void text_put(int col, int row, int entry){
  if ((unsigned)col >= TEXT_COLS || (unsigned)row >= TEXT_BORDER_ROW) return;
  unsigned short* want = (unsigned short*)text_want;
  want[row * TEXT_COLS + col] = (unsigned short)entry;
}

/* Write string `s` starting at tile (col, row) in RGB332 `color`. */
void tile_text(int col, int row, char* s, int color){
  for (; *s; ++s, ++col){
    int c = *s;
    if (c < 32 || c > 127) c = '?';
    if (c != ' ') text_put(col, row, c | (color << 8));
  }
}

/* Write string `s` centered horizontally on tile row `row` in RGB332 `color`. */
void tile_text_centered(int row, char* s, int color){
  int len = 0;
  while (s[len]) len += 1;
  tile_text((TEXT_COLS - len) >> 1, row, s, color);
}

/* Place the 24x3 tile metallic SKYFORGE logo centered on tile row `row`. */
static void place_logo_banner(int row){
  int col0 = (TEXT_COLS - LOGO_COLS) >> 1;
  for (int tr = 0; tr < LOGO_ROWS; ++tr){
    for (int tc = 0; tc < LOGO_COLS; ++tc){
      text_put(col0 + tc, row + tr, LOGO_TILE0 + tr * LOGO_COLS + tc);
    }
  }
}

/* Fill a rectangle of tiles with solid RGB332 `color` for menu panels. */
static void tile_panel_box(int c0, int r0, int w, int h, int color){
  for (int r = r0; r < r0 + h; ++r){
    for (int c = c0; c < c0 + w; ++c){
      text_put(c, r, TILE_SOLID | (color << 8));
    }
  }
}

/* Flush changed tile entries from `text_want` to the hardware tile framebuffer. */
void text_end(void){
  for (int row = 0; row < TEXT_BORDER_ROW; ++row){
    int base = row * (TEXT_COLS >> 1);
    for (int k = 0; k < (TEXT_COLS >> 1); ++k){
      unsigned w = text_want[base + k];
      if (w == text_shown[base + k]) continue;
      text_shown[base + k] = w;
      volatile unsigned* dst = (volatile unsigned*)(g_tile_fb + row * TILE_FB_STRIDE + (k << 1));
      *dst = w;
    }
  }
}

/* Fill a clipped rectangle in the 320x40 graphical cockpit HUD buffer. */
static void hud_rect(int x, int y, int w, int h, unsigned short col){
  int x1 = x + w;
  int y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > HUD_W) x1 = HUD_W;
  if (y1 > HUD_H) y1 = HUD_H;
  for (int ry = y; ry < y1; ++ry){
    unsigned short* row = g_hud + ry * HUD_W;
    for (int rx = x; rx < x1; ++rx){
      row[rx] = col;
    }
  }
}

/* Render the 320x40 graphical cockpit instrument deck and upload to rows 192..231. */
void hud_draw_panel(void){
  /* 1. Beveled dark metallic cockpit chassis */
  hud_rect(0, 0, HUD_W, HUD_H, 0x0221);
  hud_rect(0, 0, HUD_W, 2, 0x08B4);
  hud_rect(0, 2, HUD_W, 1, 0x0453);

  /* 2. Left Pod (x = 6..41, y = 4..37): 34x34 Live Topographic Minimap Radar */
  hud_rect(5, 3, 36, 36, 0x0684);
  unsigned short* pal = &biome_palettes[(g_biome & 7) << 8];
  for (int ry = 0; ry < 32; ++ry){
    int wy = (ry << 2) & MAP_MASK;
    unsigned short* dst_row = g_hud + (5 + ry) * HUD_W + 7;
    for (int rx = 0; rx < 32; ++rx){
      int wx = (rx << 2) & MAP_MASK;
      unsigned short cell = g_terrain[(wy << MAP_SHIFT) | wx];
      dst_row[rx] = pal[cell >> 8];
    }
  }
  /* Draw entity blips on the radar */
  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    int rx = 7 + (((e->x >> FIX_SHIFT) & MAP_MASK) >> 2);
    int ry = 5 + (((e->y >> FIX_SHIFT) & MAP_MASK) >> 2);
    unsigned short blip = 0x04FF;
    if (e->kind >= ENT_SCOUT && e->kind <= ENT_STEALTH) blip = 0x024F;
    else if (e->kind == ENT_BOSS) blip = 0x0F3F;
    else if (e->kind == ENT_RING || e->kind == ENT_GATE) blip = 0x04FF;
    else continue;
    g_hud[ry * HUD_W + rx] = blip;
  }
  /* Draw player blip and heading tick on radar */
  int px = 7 + (((g_player.x >> FIX_SHIFT) & MAP_MASK) >> 2);
  int py = 5 + (((g_player.y >> FIX_SHIFT) & MAP_MASK) >> 2);
  g_hud[py * HUD_W + px] = 0x0FFF;
  int hx = px + ((icos(g_player.angle) * 3) >> FIX_SHIFT);
  int hy = py + ((isin(g_player.angle) * 3) >> FIX_SHIFT);
  if (hx >= 7 && hx < 39 && hy >= 5 && hy < 37){
    g_hud[hy * HUD_W + hx] = 0x0FF2;
  }

  /* 3. Artificial Horizon / Attitude Indicator (x = 46..79, y = 4..37) */
  hud_rect(45, 3, 34, 36, 0x0575);
  int adi_pitch = g_player.pitch >> 2;
  for (int ry = 0; ry < 32; ++ry){
    unsigned short* dst_row = g_hud + (5 + ry) * HUD_W + 47;
    for (int rx = 0; rx < 30; ++rx){
      int roll_tilt = ((rx - 15) * g_player.roll) >> 5;
      int horizon = 16 + adi_pitch + roll_tilt;
      unsigned short c = (ry < horizon) ? 0x0952 : 0x0263;
      if (ry == horizon) c = 0x0FFF;
      else if (ry == 16 && (rx >= 10 && rx <= 20)) c = 0x04FF;
      dst_row[rx] = c;
    }
  }

  /* 4. Center Tactical Console & Graphical Bars (x = 84..235) */
  hud_rect(84, 4, 152, 34, 0x0110);
  /* Shield bar on tile row 25 (HUD y = 8..14) */
  int sh_w = (g_player.hp * 84) / g_player.max_hp;
  if (sh_w < 0) sh_w = 0;
  if (sh_w > 84) sh_w = 84;
  hud_rect(134, 8, 92, 6, 0x0333);
  hud_rect(135, 9, sh_w, 4, (g_player.hp > 30) ? 0x09F3 : 0x024F);

  /* Energy / Afterburner bar on tile row 26 (HUD y = 16..22) */
  int en_w = (g_player.energy * 84) / 100;
  if (en_w < 0) en_w = 0;
  if (en_w > 84) en_w = 84;
  hud_rect(134, 16, 92, 6, 0x0333);
  hud_rect(135, 17, en_w, 4, 0x02DF);

  /* Seismic Bomb icons on tile row 27 (HUD y = 24..30) */
  for (int b = 0; b < g_player.bombs && b < 6; ++b){
    hud_rect(135 + b * 12, 24, 9, 6, 0x048F);
    hud_rect(137 + b * 12, 25, 5, 4, 0x0FFF);
  }

  /* 5. Right Pod (x = 240..314): Altimeter & Speedometer / Forge Swatch */
  hud_rect(240, 4, 74, 34, 0x0110);
  int alt_cells = g_player.z >> FIX_SHIFT;
  int alt_h = (alt_cells * 28) / (MAX_HEIGHT + 18);
  if (alt_h < 2) alt_h = 2;
  if (alt_h > 28) alt_h = 28;
  hud_rect(298, 7, 12, 28, 0x0333);
  hud_rect(299, 35 - alt_h, 10, alt_h, 0x0FF4);

  copy_words((void*)(g_pixel_fb + HUD_Y * FB_W), g_hud_words, HUD_WORDS);
}

/* Update hardware sprite coordinates for the aiming reticle and target lock box. */
void hud_update_sprites(void){
  if (g_state != STATE_PLAY){
    set_sprite_coords(SPRITE_RETICLE, SPRITE_OFFSCREEN, SPRITE_OFFSCREEN);
    set_sprite_coords(SPRITE_LOCK_BOX, SPRITE_OFFSCREEN, SPRITE_OFFSCREEN);
    return;
  }

  /* Screen is 640x480 (pixel layer at 2x). Viewport center is (320, 192). */
  int ret_x = 320 + (g_player.aim_x << 1) - 16;
  int ret_y = 192 + (g_player.aim_y << 1) - 16;
  set_sprite_coords(SPRITE_RETICLE, (unsigned)ret_x, (unsigned)ret_y);

  if (g_player.lock_target >= 0 && g_entities[g_player.lock_target].active){
    set_sprite_coords(SPRITE_LOCK_BOX, (unsigned)ret_x, (unsigned)ret_y);
  } else {
    set_sprite_coords(SPRITE_LOCK_BOX, SPRITE_OFFSCREEN, SPRITE_OFFSCREEN);
  }
}

/* Hide all hardware sprites when exiting to the shell. */
void hud_hide_sprites(void){
  for (int s = 0; s < NUM_SPRITES; ++s){
    set_sprite_coords(s, SPRITE_OFFSCREEN, SPRITE_OFFSCREEN);
  }
}

/* Names of the 5 sectors and 7 Terrain Forge tools (all bounded to fit HUD). */
static char* sector_names[5] = {
  "SECTOR 1: EMERALD BASIN",
  "SECTOR 2: OBSIDIAN CALDERA",
  "SECTOR 3: CRYO-SPIRE SEA",
  "SECTOR 4: CYBER-TRENCH",
  "SECTOR 5: LEVIATHAN CORE"
};

static char* tool_names[7] = {
  "1:RAISE PEAK",
  "2:CARVE GORGE",
  "3:SMOOTH MESA",
  "4:VOLCANO RIM",
  "5:CRYSTAL GEM",
  "6:LAVA RIVER",
  "7:SPAWN RING"
};

/* Render all tile-layer text overlays for the active screen state. */
void hud_draw_overlays(int menu_cursor){
  char num[16];
  text_begin();

  if (g_state == STATE_TITLE){
    place_logo_banner(1);
    tile_text_centered(4, "3D VOXEL FLIGHT * GRAND PRIX * SCULPTOR", 0x9F);
    tile_text_centered(5, "SIGNED BY: GEMINI-4-ARGON", 0xFC);

    tile_panel_box(2, 7, 36, 12, 0x04);
    char* items[6] = {
      "[1] LAUNCH CAMPAIGN (5 WORLDS)",
      "[2] CANYON GRAND PRIX (24 GATE)",
      "[3] TERRAIN FORGE (3D SCULPTOR)",
      "[4] AI WINGMAN AUTOPILOT DEMO",
      "[5] FLIGHT MANUAL & CONTROLS",
      "[Q] EXIT TO DIOPTASE SHELL"
    };
    for (int i = 0; i < 6; ++i){
      int row = 8 + (i << 1);
      if (i == menu_cursor){
        tile_panel_box(3, row, 34, 1, 0x16);
        tile_text(4, row, ">", 0xFC);
      }
      tile_text(6, row, items[i], (i == menu_cursor) ? 0xFF : 0xDF);
    }

    tile_text(5, 20, "HIGH SCORE:", 0xFC);
    tile_text(17, 20, fmt_int(num, g_high_scores[0]), 0xFF);
    if (g_best_lap_ticks > 0){
      tile_text(24, 20, "BEST RACE:", 0x7F);
      tile_text(35, 20, fmt_int(num, g_best_lap_ticks / 60), 0xFF);
    }
    tile_text_centered(22, "UP/DOWN + ENTER OR CLICK TO SELECT", 0x7F);
  } else if (g_state == STATE_HELP){
    tile_panel_box(2, 2, 36, 20, 0x04);
    tile_text_centered(3, "SKYFORGE FLIGHT & SCULPTOR MANUAL", 0xFC);
    tile_text_centered(4, "CREATED BY GEMINI-4-ARGON", 0x7F);
    tile_text(4, 6,  "WASD / ARROWS : PITCH & BANK TURN", 0xFF);
    tile_text(4, 8,  "MOUSE / CLICK : AIM RETICLE & FIRE", 0xFF);
    tile_text(4, 10, "SPACE / LMB   : TWIN PLASMA CANNONS", 0x9F);
    tile_text(4, 12, "F / RMB       : SEISMIC CRATER BOMB", 0xFC);
    tile_text(4, 14, "E / SHIFT     : BARREL ROLL / BOOST", 0x7F);
    tile_text(4, 16, "V: COCKPIT CAM  D: AI WINGMAN PILOT", 0xDF);
    tile_text(4, 18, "FORGE: 1-7 TOOL  B: BIOME  O/L: SAVE", 0x9F);
    tile_text_centered(20, "PRESS ENTER, SPACE, OR H TO RETURN", 0xFC);
  } else if (g_state == STATE_PAUSE){
    tile_panel_box(10, 8, 20, 8, 0x04);
    tile_text_centered(9,  "=== MISSION PAUSED ===", 0xFC);
    tile_text_centered(11, "P / ENTER : RESUME", 0xFF);
    tile_text_centered(13, "T : TERRAIN FORGE", 0x9F);
    tile_text_centered(15, "Q : TITLE SCREEN", 0xE5);
  } else if (g_state == STATE_SECTOR_CLEAR){
    tile_panel_box(5, 7, 30, 9, 0x04);
    tile_text_centered(8, "=== SECTOR CLEARED! ===", 0x9F);
    tile_text_centered(10, sector_names[g_sector - 1], 0xFC);
    tile_text(12, 12, "SCORE:", 0xFF);
    tile_text(19, 12, fmt_int(num, g_player.score), 0xFC);
    tile_text_centered(14, "PRESS ENTER FOR NEXT SECTOR", 0x7F);
  } else if (g_state == STATE_VICTORY){
    tile_panel_box(4, 6, 32, 11, 0x04);
    tile_text_centered(7, "*** VICTORY OVER DIOPTASE! ***", 0xFC);
    tile_text_centered(9, "THE SKYFORGE REALM IS SECURED!", 0x9F);
    tile_text(11, 11, "FINAL SCORE:", 0xFF);
    tile_text(24, 11, fmt_int(num, g_player.score), 0xFC);
    tile_text_centered(13, "SIGNED BY GEMINI-4-ARGON", 0x7F);
    tile_text_centered(15, "ENTER: NEW RUN   Q: HOME MENU", 0xFF);
  } else if (g_state == STATE_GAMEOVER){
    tile_panel_box(6, 7, 28, 9, 0x20);
    tile_text_centered(8, "=== INTERCEPTOR DOWN ===", 0xE0);
    tile_text(11, 11, "FINAL SCORE:", 0xFF);
    tile_text(24, 11, fmt_int(num, g_player.score), 0xFC);
    tile_text_centered(14, "ENTER: RETRY   Q: HOME MENU", 0xFF);
  } else {
    /* Active Gameplay HUD Header (Rows 0..2) */
    if (g_mode == MODE_CAMPAIGN){
      tile_text(1, 0, sector_names[g_sector - 1], 0x9F);
      tile_text(29, 0, "FOES:", 0xFC);
      tile_text(35, 0, fmt_int(num, world_enemies_remaining()), 0xFF);
    } else if (g_mode == MODE_RACE){
      tile_text(1, 0, "CANYON GRAND PRIX", 0xFC);
      tile_text(20, 0, "GATE:", 0x9F);
      tile_text(26, 0, fmt_int(num, g_player.next_gate + 1), 0xFF);
      tile_text(28, 0, "/24", 0x9F);
      tile_text(33, 0, "T:", 0x7F);
      tile_text(35, 0, fmt_int(num, g_player.race_ticks / 60), 0xFF);
    } else {
      tile_text(1, 0, "TERRAIN FORGE", 0xFC);
      tile_text(16, 0, tool_names[g_player.forge_tool], 0x9F);
      tile_text(31, 0, "RAD:", 0x7F);
      tile_text(36, 0, fmt_int(num, g_player.forge_radius), 0xFF);
      tile_text_centered(22, "[LMB/SPC] SCULPT [1-7] TOOL [B] BIOME", 0xDF);
    }

    if (g_autopilot){
      tile_text_centered(1, "[AI WINGMAN ACTIVE - PRESS D FOR MANUAL]", 0x7F);
    } else if (g_banner_timer > 0){
      tile_text_centered(2, g_banner_msg, 0xFC);
    }
  }

  /* Cockpit Instrument Labels (Rows 25..27 aligned with HUD bars at y = 8, 16, 24) */
  tile_text(11, 25, "SHLD", 0x9F);
  tile_text(11, 26, "BOST", 0xFC);
  tile_text(11, 27, "BOMB", 0x7F);
  tile_text(30, 25, "SCORE", 0xFC);
  tile_text(30, 26, fmt_int(num, g_player.score), 0xFF);
  tile_text(30, 27, "WPN L", 0x9F);
  tile_text(35, 27, fmt_int(num, g_player.weapon_level), 0xFF);

  text_end();
}

#endif /* !SKYFORGE_HOST_TEST */
