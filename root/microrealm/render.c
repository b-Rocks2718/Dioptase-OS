#include "realm.h"
#include "assets.h"
#include "../crt/sys.h"
#include "../crt/string.h"

static volatile short* fb = NULL;

/* Borrow the process's user-mode pixel mapping; only rows 0..235 are drawn.
 * The CRT owns the trap transition. Mapping failure leaves rendering disabled.
 */
void render_init(void) {
  fb = get_pixel_fb();
  if (fb == (volatile short*)-1) fb = NULL;
  if (fb != NULL) set_pixel_scale(0);
}

/* Fast plot of an individual 12-bit color pixel using bit-shift addressing. */
static void plot_pixel(int x, int y, short color) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= MAX_RENDER_Y) return;
  if (color == COLOR_TRANSPARENT) return;
  if (fb != NULL) {
    /* 320 = 256 + 64 = (y << 8) + (y << 6) */
    fb[((y << 8) + (y << 6)) + x] = color;
  }
}

/* Fill an axis-aligned rectangular block of pixels with pointer stepping. */
static void fill_rect(int x, int y, int w, int h, short color) {
  if (color == COLOR_TRANSPARENT) return;
  int x0 = (x < 0) ? 0 : x;
  int y0 = (y < 0) ? 0 : y;
  int x1 = (x + w > SCREEN_W) ? SCREEN_W : (x + w);
  int y1 = (y + h > MAX_RENDER_Y) ? MAX_RENDER_Y : (y + h);

  if (fb != NULL && x0 < x1 && y0 < y1) {
    int row_off = (y0 << 8) + (y0 << 6);
    int span = x1 - x0;
    for (int cy = y0; cy < y1; cy++) {
      volatile short* dst = &fb[row_off + x0];
      int remaining = span;
      /* Eight ordinary 16-bit stores amortize loop control without changing
       * access width or requiring word alignment at clipped rectangle edges.
       */
      while (remaining >= 8) {
        dst[0] = color;
        dst[1] = color;
        dst[2] = color;
        dst[3] = color;
        dst[4] = color;
        dst[5] = color;
        dst[6] = color;
        dst[7] = color;
        dst += 8;
        remaining -= 8;
      }
      while (remaining > 0) {
        *dst++ = color;
        remaining--;
      }
      row_off += SCREEN_W;
    }
  }
}

/* Draw a hollow rectangular wireframe box. */
static void draw_box(int x, int y, int w, int h, short color) {
  for (int cx = x; cx < x + w; cx++) {
    plot_pixel(cx, y, color);
    plot_pixel(cx, y + h - 1, color);
  }
  for (int cy = y; cy < y + h; cy++) {
    plot_pixel(x, cy, color);
    plot_pixel(x + w - 1, cy, color);
  }
}

/* Clip each glyph column and write only opaque pixels into a valid mapping. */
static void draw_char(int x, int y, char c, short color) {
  if (fb == NULL || color == COLOR_TRANSPARENT || c < 32 || c > 126) return;
  int glyph_idx = c - 32;

  for (int col = 0; col < 5; col++) {
    int px = x + col;
    if (px >= 0 && px < SCREEN_W) {
      unsigned char bits = font5x7[glyph_idx][col];
      for (int row = 0; row < 7; row++) {
        int py = y + row;
        if (py >= 0 && py < MAX_RENDER_Y && ((bits >> row) & 1)) {
          fb[((py << 8) + (py << 6)) + px] = color;
        }
      }
    }
  }
}

/* Render a null-terminated string using the 5x7 bitmap font. */
static void draw_string(int x, int y, char* str, short color) {
  int cx = x;
  int idx = 0;
  while (str[idx] != '\0') {
    draw_char(cx, y, str[idx], color);
    cx += 6;
    idx++;
  }
}

/* Draw all 32-bit signed values without division, including the minimum int.
 * Unsigned subtraction forms its magnitude without overflowing signed negation;
 * each decimal position requires at most nine subtractions.
 */
static void draw_number(int x, int y, int val, short color) {
  unsigned magnitude = (unsigned)val;
  if (val < 0) {
    draw_char(x, y, '-', color);
    x += 6;
    magnitude = 0u - magnitude;
  }
  static unsigned powers10[10] = {
    1000000000, 100000000, 10000000, 1000000, 100000,
    10000, 1000, 100, 10, 1
  };
  int started = 0;
  for (int i = 0; i < 10; i++) {
    unsigned p = powers10[i];
    int digit = 0;
    while (magnitude >= p) {
      magnitude -= p;
      digit++;
    }
    if (digit > 0 || started || i == 9) {
      started = 1;
      draw_char(x, y, '0' + digit, color);
      x += 6;
    }
  }
}

/* Render a 16x16 natural resource node (tree, stone rock, or gold vein). */
static void draw_resource_tile(int sx, int sy, int res_type) {
  if (res_type == RES_TREE) {
    fill_rect(sx + 7, sy + 10, 3, 6, PAL_WOOD_BARK);
    fill_rect(sx + 4, sy + 3, 9, 8, PAL_LEAVES_DEEP);
    fill_rect(sx + 6, sy + 1, 5, 3, PAL_LEAVES_LGT);
    fill_rect(sx + 3, sy + 5, 2, 4, PAL_LEAVES_LGT);
    fill_rect(sx + 12, sy + 5, 2, 4, PAL_LEAVES_DEEP);
  } else if (res_type == RES_STONE) {
    fill_rect(sx + 3, sy + 6, 11, 8, PAL_STONE_DARK);
    fill_rect(sx + 5, sy + 4, 7, 3, PAL_STONE_LGT);
    plot_pixel(sx + 6, sy + 7, PAL_WHITE);
    plot_pixel(sx + 9, sy + 9, PAL_STONE_LGT);
  } else if (res_type == RES_GOLD) {
    fill_rect(sx + 3, sy + 6, 11, 8, PAL_GOLD_DARK);
    fill_rect(sx + 5, sy + 4, 7, 3, PAL_GOLD_BRIGHT);
    plot_pixel(sx + 5, sy + 6, PAL_WHITE);
    plot_pixel(sx + 9, sy + 7, PAL_WHITE);
    plot_pixel(sx + 7, sy + 10, PAL_GOLD_BRIGHT);
  }
}

/* Render a building structure onto the viewport canvas. */
static void draw_building_sprite(int sx, int sy, struct Building* b) {
  int bw = b->width << 4;
  int bh = b->height << 4;

  switch (b->type) {
    case BLD_CASTLE:
      fill_rect(sx, sy, bw, bh, PAL_STONE_DARK);
      fill_rect(sx + 2, sy + 2, 8, bh - 4, PAL_STONE_LGT);
      fill_rect(sx + bw - 10, sy + 2, 8, bh - 4, PAL_STONE_LGT);
      fill_rect(sx + 14, sy + 4, 20, 16, PAL_STONE_LGT);
      fill_rect(sx + 6, sy + 8, 4, 12, PAL_BANNER_BLUE);
      fill_rect(sx + bw - 10, sy + 8, 4, 12, PAL_BANNER_BLUE);
      fill_rect(sx + 18, sy + bh - 18, 12, 18, PAL_WOOD_BARK);
      break;

    case BLD_HOUSE:
      fill_rect(sx + 2, sy + 12, bw - 4, bh - 14, PAL_DIRT_LIGHT);
      for (int i = 0; i < 12; i++) {
        fill_rect(sx + i, sy + 12 - i, bw - (i << 1), 2, PAL_ROOF_RED);
      }
      fill_rect(sx + 12, sy + bh - 10, 8, 10, PAL_WOOD_BARK);
      fill_rect(sx + 4, sy + 16, 5, 5, PAL_BANNER_BLUE);
      break;

    case BLD_FARM:
      fill_rect(sx + 1, sy + 1, bw - 2, bh - 2, PAL_DIRT);
      for (int f = 0; f < 3; f++) {
        fill_rect(sx + 4, sy + 4 + (f << 3), bw - 8, 4, PAL_WHEAT);
      }
      draw_box(sx, sy, bw, bh, PAL_WOOD_BARK);
      break;

    case BLD_LUMBER_CAMP:
      fill_rect(sx + 2, sy + 8, bw - 4, bh - 10, PAL_WOOD_BARK);
      fill_rect(sx, sy + 2, bw, 6, PAL_LEAVES_DEEP);
      fill_rect(sx + 18, sy + 16, 10, 12, PAL_DIRT_LIGHT);
      break;

    case BLD_BARRACKS:
      fill_rect(sx + 2, sy + 8, bw - 4, bh - 10, PAL_STONE_DARK);
      fill_rect(sx + 4, sy + 2, bw - 8, 6, PAL_STEEL);
      fill_rect(sx + 10, sy + bh - 14, 10, 14, PAL_WOOD_BARK);
      fill_rect(sx + 28, sy + 12, 8, 8, PAL_BANNER_BLUE);
      plot_pixel(sx + 31, sy + 15, PAL_WHITE);
      plot_pixel(sx + 32, sy + 16, PAL_WHITE);
      break;

    case BLD_TOWER:
      fill_rect(sx + 4, sy + 6, bw - 8, bh - 6, PAL_STONE_DARK);
      fill_rect(sx + 2, sy + 1, bw - 4, 6, PAL_STONE_LGT);
      plot_pixel(sx + 3, sy, PAL_STONE_LGT);
      plot_pixel(sx + bw - 4, sy, PAL_STONE_LGT);
      fill_rect(sx + 14, sy + 12, 3, 7, PAL_BLACK);
      break;

    case BLD_WALL:
      fill_rect(sx, sy, bw, bh, PAL_STONE_DARK);
      draw_box(sx, sy, bw, bh, PAL_STONE_LGT);
      break;

    default:
      fill_rect(sx, sy, bw, bh, PAL_GRAY);
      break;
  }

  /* Structural health gauge if damaged */
  if (b->hp < b->max_hp) {
    int bar_w = bw - 4;
    int filled = (b->hp * bar_w) / b->max_hp;
    fill_rect(sx + 2, sy - 5, bar_w, 3, PAL_BLACK);
    fill_rect(sx + 2, sy - 5, filled, 3, (filled > bar_w / 3) ? PAL_GRASS_LIGHT : PAL_CRIMSON);
  }
}

/* Render animated 16x16 humanoid and monster sprites. */
static void draw_unit_sprite(int sx, int sy, struct Unit* u) {
  int bounce = (u->state == STATE_WALK) ? (u->anim_frame ? 1 : 0) : 0;
  int y = sy - bounce;

  switch (u->type) {
    case UNIT_VILLAGER:
      fill_rect(sx + 5, y + 6, 6, 7, PAL_LEATHER);
      fill_rect(sx + 6, y + 2, 4, 4, PAL_SKIN_TONE);
      plot_pixel(sx + 7, y + 1, PAL_DIRT);
      fill_rect(sx + (u->facing_left ? 3 : 11), y + 4, 2, 8, PAL_WOOD_BARK);
      plot_pixel(sx + (u->facing_left ? 2 : 12), y + 3, PAL_STEEL);
      break;

    case UNIT_SWORDSMAN:
      fill_rect(sx + 5, y + 5, 6, 8, PAL_STEEL);
      fill_rect(sx + 6, y + 1, 4, 4, PAL_STEEL_BRIGHT);
      plot_pixel(sx + 7, y, PAL_BANNER_BLUE);
      fill_rect(sx + (u->facing_left ? 11 : 2), y + 6, 3, 7, PAL_BANNER_BLUE);
      fill_rect(sx + (u->facing_left ? 2 : 12), y + 2, 2, 9, PAL_STEEL_BRIGHT);
      break;

    case UNIT_ARCHER:
      fill_rect(sx + 5, y + 6, 6, 7, PAL_LEAVES_DEEP);
      fill_rect(sx + 6, y + 2, 4, 4, PAL_SKIN_TONE);
      plot_pixel(sx + 7, y + 1, PAL_LEAVES_LGT);
      fill_rect(sx + (u->facing_left ? 2 : 12), y + 2, 2, 10, PAL_WOOD_BARK);
      break;

    case UNIT_GOBLIN:
      fill_rect(sx + 5, y + 7, 5, 6, PAL_GOBLIN_SKIN);
      fill_rect(sx + 6, y + 3, 4, 4, PAL_GOBLIN_SKIN);
      plot_pixel(sx + 5, y + 2, PAL_GOBLIN_SKIN);
      plot_pixel(sx + 10, y + 2, PAL_GOBLIN_SKIN);
      plot_pixel(sx + 7, y + 4, PAL_CRIMSON);
      break;

    case UNIT_ORC:
      fill_rect(sx + 4, y + 5, 8, 8, PAL_ORC_SKIN);
      fill_rect(sx + 5, y + 1, 6, 5, PAL_ORC_SKIN);
      plot_pixel(sx + 6, y + 3, PAL_CRIMSON);
      fill_rect(sx + (u->facing_left ? 1 : 12), y + 1, 3, 11, PAL_STEEL);
      break;

    case UNIT_SKELETON:
      fill_rect(sx + 6, y + 6, 4, 7, PAL_BONE);
      fill_rect(sx + 6, y + 2, 4, 4, PAL_BONE);
      plot_pixel(sx + 7, y + 3, PAL_BLACK);
      fill_rect(sx + (u->facing_left ? 2 : 11), y + 2, 2, 9, PAL_DARK_GRAY);
      break;

    case UNIT_OGRE:
      fill_rect(sx + 2, y + 3, 12, 12, PAL_OGRE_SKIN);
      fill_rect(sx + 5, y, 6, 5, PAL_OGRE_SKIN);
      fill_rect(sx + (u->facing_left ? 0 : 13), y - 2, 4, 14, PAL_WOOD_BARK);
      break;

    case UNIT_BOSS:
      fill_rect(sx + 3, y + 2, 10, 13, PAL_BOSS_PURPLE);
      fill_rect(sx + 5, y - 2, 6, 5, PAL_BOSS_PURPLE);
      plot_pixel(sx + 4, y - 4, PAL_CRIMSON);
      plot_pixel(sx + 11, y - 4, PAL_CRIMSON);
      plot_pixel(sx + 6, y, PAL_CRIMSON);
      plot_pixel(sx + 9, y, PAL_CRIMSON);
      break;

    default:
      fill_rect(sx + 5, y + 5, 6, 7, PAL_WHITE);
      break;
  }

  /* Selection halo indicator */
  if (u->selected) {
    draw_box(sx + 2, sy + 13, 12, 4, PAL_GOLD_BRIGHT);
  }

  /* Health gauge */
  if (u->hp < u->max_hp) {
    int filled = (u->hp * 12) / u->max_hp;
    fill_rect(sx + 2, sy - 4, 12, 2, PAL_BLACK);
    fill_rect(sx + 2, sy - 4, filled, 2, (u->team == TEAM_PLAYER) ? PAL_GRASS_LIGHT : PAL_CRIMSON);
  }
}

/* Render tactical radar minimap with bit-shift coordinate scaling. */
static void render_minimap(struct GameWorld* w) {
  int mx = 3;
  int my = HUD_Y + 2;
  int mw = 24;
  int mh = 24;

  draw_box(mx - 1, my - 1, mw + 2, mh + 2, PAL_HUD_BORDER);
  fill_rect(mx, my, mw, mh, PAL_WATER_DEEP);

  /* Landmass dot sampling: tx * 24 / 64 = (tx * 3) >> 3 */
  for (int ty = 0; ty < MAP_SIZE; ty += 4) {
    int py = my + ((ty * 3) >> 3);
    for (int tx = 0; tx < MAP_SIZE; tx += 4) {
      struct MapCell* c = world_get_cell(w, tx, ty);
      if (c != NULL && c->terrain != TERRAIN_WATER) {
        int px = mx + ((tx * 3) >> 3);
        plot_pixel(px, py, PAL_GRASS_BASE);
      }
    }
  }

  /* Friendly structures */
  for (int b = 0; b < MAX_BUILDINGS; b++) {
    struct Building* bld = &w->buildings[b];
    if (bld->active) {
      plot_pixel(mx + ((bld->tile_x * 3) >> 3), my + ((bld->tile_y * 3) >> 3), PAL_WHITE);
    }
  }

  /* Units: World is 1024 (>> 10); (x * 24) >> 10 = (x * 3) >> 7 */
  for (int u = 0; u < TOTAL_UNITS; u++) {
    struct Unit* unit = &w->units[u];
    if (!unit->active || unit->hp <= 0) continue;
    int px = ((unit->x >> FIXED_SHIFT) * 3) >> 7;
    int py = ((unit->y >> FIXED_SHIFT) * 3) >> 7;
    plot_pixel(mx + px, my + py, (unit->team == TEAM_PLAYER) ? PAL_GOLD_BRIGHT : PAL_CRIMSON);
  }

  /* Viewport camera bounds rectangle */
  int cam_mx = (w->cam_x * 3) >> 7;
  int cam_my = (w->cam_y * 3) >> 7;
  int cam_mw = (SCREEN_W * 3) >> 7;
  int cam_mh = (VIEW_H * 3) >> 7;
  draw_box(mx + cam_mx, my + cam_my, (cam_mw < 3) ? 3 : cam_mw, (cam_mh < 3) ? 3 : cam_mh, PAL_WHITE);
}

/* Render HUD command console and resource telemetry along bottom rows. */
static void render_hud(struct GameWorld* w) {
  fill_rect(0, HUD_Y, SCREEN_W, HUD_H, PAL_HUD_BG);
  fill_rect(0, HUD_Y, SCREEN_W, 1, PAL_HUD_BORDER);

  render_minimap(w);

  /* Row 1: Day progression and phase */
  draw_string(32, HUD_Y + 3, "DAY", PAL_GOLD_BRIGHT);
  draw_number(52, HUD_Y + 3, w->day_number, PAL_WHITE);

  if (w->phase == PHASE_DAWN)       draw_string(72, HUD_Y + 3, "[DAWN]", PAL_GOLD_BRIGHT);
  else if (w->phase == PHASE_DAY)   draw_string(72, HUD_Y + 3, "[DAY]", PAL_GRASS_LIGHT);
  else if (w->phase == PHASE_DUSK)  draw_string(72, HUD_Y + 3, "[DUSK]", COLOR_RGB(15, 8, 1));
  else                              draw_string(72, HUD_Y + 3, "[NIGHT]", PAL_CRIMSON);

  draw_string(114, HUD_Y + 3, "SCORE:", PAL_LIGHT_GRAY);
  draw_number(150, HUD_Y + 3, w->score, PAL_WHITE);

  /* Row 2: Stockpile counts */
  draw_string(32, HUD_Y + 11, "W:", PAL_WOOD_BARK);
  draw_number(44, HUD_Y + 11, w->wood, PAL_WHITE);

  draw_string(68, HUD_Y + 11, "S:", PAL_STONE_LGT);
  draw_number(80, HUD_Y + 11, w->stone, PAL_WHITE);

  draw_string(104, HUD_Y + 11, "G:", PAL_GOLD_BRIGHT);
  draw_number(116, HUD_Y + 11, w->gold, PAL_WHITE);

  draw_string(140, HUD_Y + 11, "F:", PAL_WHEAT);
  draw_number(152, HUD_Y + 11, w->food, PAL_WHITE);

  draw_string(176, HUD_Y + 11, "POP:", PAL_LIGHT_GRAY);
  draw_number(200, HUD_Y + 11, w->population, PAL_WHITE);
  draw_char(214, HUD_Y + 11, '/', PAL_LIGHT_GRAY);
  draw_number(222, HUD_Y + 11, w->max_population, PAL_WHITE);

  /* Row 3: Action and Build Shortcuts */
  draw_string(32, HUD_Y + 19, "[H]OUSE [F]ARM [T]OWER [B]ARRACK [W]ALL [D]EMO", PAL_BANNER_BLUE);
  if (w->auto_commander) {
    draw_string(274, HUD_Y + 19, "[AI:ON]", PAL_GRASS_LIGHT);
  } else {
    draw_string(274, HUD_Y + 19, "[AI:OFF]", PAL_DARK_GRAY);
  }
}

/* Primary viewport composition pass executed every presentation tick. */
void render_frame(struct GameWorld* w, int mouse_px, int mouse_py, int drag_active, int drag_x0, int drag_y0) {
  /* Fast shifts: TILE_PX = 16 = >> 4 */
  int min_tx = w->cam_x >> 4;
  int min_ty = w->cam_y >> 4;
  int max_tx = ((w->cam_x + SCREEN_W) >> 4) + 1;
  int max_ty = ((w->cam_y + VIEW_H) >> 4) + 1;
  if (min_tx < 0) min_tx = 0;
  if (min_ty < 0) min_ty = 0;
  if (max_tx > MAP_SIZE) max_tx = MAP_SIZE;
  if (max_ty > MAP_SIZE) max_ty = MAP_SIZE;

  /* Render visible terrain grid */
  for (int ty = min_ty; ty < max_ty; ty++) {
    int sy = (ty << 4) - w->cam_y;
    int row_start = ty << 6;
    for (int tx = min_tx; tx < max_tx; tx++) {
      struct MapCell* cell = &w->grid[row_start + tx];
      int sx = (tx << 4) - w->cam_x;

      short tile_color = PAL_GRASS_BASE;
      if (cell->terrain == TERRAIN_WATER) {
        tile_color = PAL_WATER_DEEP;
      } else if (cell->terrain == TERRAIN_SHORE) {
        tile_color = PAL_SAND;
      } else if (cell->terrain == TERRAIN_DIRT) {
        tile_color = PAL_DIRT;
      }

      fill_rect(sx, sy, 16, 16, tile_color);

      /* Subtle ground texture flecks */
      if (cell->terrain == TERRAIN_GRASS && ((tx + ty) & 3) == 0) {
        plot_pixel(sx + 3, sy + 3, PAL_GRASS_LIGHT);
      }

      /* Resource node atop terrain */
      if (cell->resource != RES_NONE) {
        draw_resource_tile(sx, sy, cell->resource);
      }
    }
  }

  /* Render buildings */
  for (int b = 0; b < MAX_BUILDINGS; b++) {
    struct Building* bld = &w->buildings[b];
    if (!bld->active) continue;

    int sx = (bld->tile_x << 4) - w->cam_x;
    int sy = (bld->tile_y << 4) - w->cam_y;
    int bw = bld->width << 4;
    int bh = bld->height << 4;

    if (sx + bw >= 0 && sx < SCREEN_W && sy + bh >= 0 && sy < VIEW_H) {
      draw_building_sprite(sx, sy, bld);
    }
  }

  /* Render units */
  for (int u = 0; u < TOTAL_UNITS; u++) {
    struct Unit* unit = &w->units[u];
    if (!unit->active || unit->hp <= 0) continue;

    int ux = (unit->x >> FIXED_SHIFT) - 8;
    int uy = (unit->y >> FIXED_SHIFT) - 8;
    int sx = ux - w->cam_x;
    int sy = uy - w->cam_y;

    if (sx >= -16 && sx < SCREEN_W && sy >= -16 && sy < VIEW_H) {
      draw_unit_sprite(sx, sy, unit);
    }
  }

  /* Render projectiles */
  for (int p = 0; p < MAX_PROJECTILES; p++) {
    struct Projectile* prj = &w->projectiles[p];
    if (!prj->active) continue;

    int px = (prj->x >> FIXED_SHIFT) - w->cam_x;
    int py = (prj->y >> FIXED_SHIFT) - w->cam_y - (prj->z >> FIXED_SHIFT);
    if (px >= 0 && px < SCREEN_W && py >= 0 && py < VIEW_H) {
      plot_pixel(px, py, (prj->team == TEAM_PLAYER) ? PAL_WHITE : PAL_CRIMSON);
      plot_pixel(px + 1, py, PAL_LIGHT_GRAY);
    }
  }

  /* Render particles */
  for (int pt = 0; pt < MAX_PARTICLES; pt++) {
    struct Particle* part = &w->particles[pt];
    if (!part->active) continue;

    int px = (part->x >> FIXED_SHIFT) - w->cam_x;
    int py = (part->y >> FIXED_SHIFT) - w->cam_y;
    plot_pixel(px, py, part->color);
  }

  /* Render floating texts */
  for (int ft = 0; ft < MAX_FLOAT_TEXTS; ft++) {
    struct FloatText* txt = &w->float_texts[ft];
    if (!txt->active) continue;

    int tx = txt->x - w->cam_x;
    int ty = txt->y - w->cam_y;
    if (tx >= 0 && tx < SCREEN_W - 50 && ty >= 0 && ty < VIEW_H) {
      draw_string(tx, ty, txt->text, txt->color);
    }
  }

  /* Building placement preview ghost */
  if (w->selected_building_type != BLD_NONE) {
    int gtx = (mouse_px + w->cam_x) >> 4;
    int gty = (mouse_py + w->cam_y) >> 4;
    int gsx = (gtx << 4) - w->cam_x;
    int gsy = (gty << 4) - w->cam_y;

    bool valid = world_can_place(w, w->selected_building_type, gtx, gty);
    short ghost_col = valid ? PAL_GRASS_LIGHT : PAL_CRIMSON;
    draw_box(gsx, gsy, 32, 32, ghost_col);
  }

  /* Selection drag bounding rectangle */
  if (drag_active) {
    int x0 = (drag_x0 < mouse_px) ? drag_x0 : mouse_px;
    int y0 = (drag_y0 < mouse_py) ? drag_y0 : mouse_py;
    int w_box = (drag_x0 < mouse_px) ? (mouse_px - drag_x0) : (drag_x0 - mouse_px);
    int h_box = (drag_y0 < mouse_py) ? (mouse_py - drag_y0) : (drag_y0 - mouse_py);
    draw_box(x0, y0, w_box, h_box, PAL_GRASS_LIGHT);
  }

  /* Render command console */
  render_hud(w);

  /* Hardware mouse cursor arrow */
  if (mouse_px >= 0 && mouse_px < SCREEN_W && mouse_py >= 0 && mouse_py < MAX_RENDER_Y) {
    plot_pixel(mouse_px, mouse_py, PAL_WHITE);
    plot_pixel(mouse_px + 1, mouse_py + 1, PAL_WHITE);
    plot_pixel(mouse_px + 2, mouse_py + 2, PAL_WHITE);
    plot_pixel(mouse_px + 3, mouse_py + 3, PAL_WHITE);
    plot_pixel(mouse_px, mouse_py + 1, PAL_GOLD_BRIGHT);
    plot_pixel(mouse_px, mouse_py + 2, PAL_GOLD_BRIGHT);
    plot_pixel(mouse_px, mouse_py + 3, PAL_GOLD_BRIGHT);
    plot_pixel(mouse_px + 1, mouse_py + 2, PAL_GOLD_BRIGHT);
  }
}

/* Render grand campaign title screen with credits and instruction prompts. */
void render_title_screen(struct GameWorld* w) {
  fill_rect(0, 0, SCREEN_W, MAX_RENDER_Y, PAL_HUD_BG);

  fill_rect(20, 20, SCREEN_W - 40, 60, PAL_HUD_PANEL);
  draw_box(20, 20, SCREEN_W - 40, 60, PAL_HUD_BORDER);

  draw_string(68, 30, "M I C R O R E A L M", PAL_GOLD_BRIGHT);
  draw_string(62, 45, "BASTION OF ANTIGRAVITY", PAL_WHITE);
  draw_string(88, 62, "CREATED BY ANTIGRAVITY", PAL_BANNER_BLUE);

  draw_string(72, 100, "[ENTER]  FOUND KINGDOM", PAL_WHITE);
  draw_string(72, 118, "[ D ]    AUTONOMOUS SPECTATOR", PAL_GRASS_LIGHT);
  draw_string(72, 136, "[ H ]    FIELD MANUAL / CONTROLS", PAL_LIGHT_GRAY);
  draw_string(72, 154, "[ Q ]    QUIT TO SHELL", PAL_CRIMSON);

  fill_rect(50, 180, SCREEN_W - 100, 36, PAL_HUD_PANEL);
  draw_box(50, 180, SCREEN_W - 100, 36, PAL_HUD_BORDER);
  draw_string(60, 186, "ROYAL HIGH RECORDS:", PAL_GOLD_BRIGHT);
  draw_string(60, 198, "BEST SURVIVED:     DAYS", PAL_LIGHT_GRAY);
  draw_number(146, 198, w->record.best_days, PAL_WHITE);
  draw_string(172, 198, "SCORE:", PAL_LIGHT_GRAY);
  draw_number(210, 198, w->record.best_score, PAL_WHITE);
}

/* Render gameplay manual and control key reference screen. */
void render_help_screen(void) {
  fill_rect(0, 0, SCREEN_W, MAX_RENDER_Y, PAL_HUD_BG);
  draw_box(10, 10, SCREEN_W - 20, MAX_RENDER_Y - 20, PAL_HUD_BORDER);

  draw_string(96, 20, "REALM FIELD MANUAL", PAL_GOLD_BRIGHT);

  draw_string(24, 40, "MOUSE CONTROLS:", PAL_WHITE);
  draw_string(28, 52, "LEFT-CLICK:   Select units / place structures", PAL_LIGHT_GRAY);
  draw_string(28, 52 + 12, "LEFT-DRAG:    Box-select multiple troops", PAL_LIGHT_GRAY);
  draw_string(28, 52 + 24, "RIGHT-CLICK:  Command move / gather / attack", PAL_LIGHT_GRAY);
  draw_string(28, 52 + 36, "MINIMAP:      Click radar to navigate camera", PAL_LIGHT_GRAY);

  draw_string(24, 106, "KEYBOARD SHORTCUTS:", PAL_WHITE);
  draw_string(28, 118, "WASD / ARROWS: Scroll camera across kingdom", PAL_LIGHT_GRAY);
  draw_string(28, 118 + 12, "H / F / T:     Build House / Farm / Guard Tower", PAL_LIGHT_GRAY);
  draw_string(28, 118 + 24, "B / W:         Build Barracks / Stone Wall", PAL_LIGHT_GRAY);
  draw_string(28, 118 + 36, "V / S / A:     Train Villager / Swordsman / Archer", PAL_LIGHT_GRAY);
  draw_string(28, 118 + 48, "D:             Toggle Autonomous AI Commander", PAL_GRASS_LIGHT);
  draw_string(28, 118 + 60, "SPACE:         Pause game    Q: Quit to shell", PAL_LIGHT_GRAY);

  draw_string(88, 204, "[PRESS ANY KEY TO RETURN]", PAL_GOLD_BRIGHT);
}

/* Render defeat screen when Castle stronghold falls. */
void render_gameover_screen(struct GameWorld* w) {
  fill_rect(40, 50, SCREEN_W - 80, 130, PAL_HUD_BG);
  draw_box(40, 50, SCREEN_W - 80, 130, PAL_CRIMSON);

  draw_string(90, 68, "THE REALM HAS FALLEN!", PAL_CRIMSON);
  draw_string(70, 90, "DAYS SURVIVED:", PAL_LIGHT_GRAY);
  draw_number(160, 90, w->day_number, PAL_WHITE);
  draw_string(70, 105, "ENEMIES SLAIN:", PAL_LIGHT_GRAY);
  draw_number(160, 105, w->enemies_slain, PAL_WHITE);
  draw_string(70, 120, "FINAL SCORE:", PAL_LIGHT_GRAY);
  draw_number(160, 120, w->score, PAL_GOLD_BRIGHT);

  draw_string(78, 150, "[ENTER] RETRY CAMPAIGN", PAL_WHITE);
}

/* Render victory screen when completing wave defense challenges. */
void render_victory_screen(struct GameWorld* w) {
  fill_rect(40, 50, SCREEN_W - 80, 130, PAL_HUD_BG);
  draw_box(40, 50, SCREEN_W - 80, 130, PAL_GOLD_BRIGHT);

  draw_string(88, 68, "VICTORY OF ANTIGRAVITY!", PAL_GOLD_BRIGHT);
  draw_string(64, 90, "THE SHADOW HORDE HAS ROUTED!", PAL_WHITE);
  draw_string(70, 108, "FINAL SCORE:", PAL_LIGHT_GRAY);
  draw_number(160, 108, w->score, PAL_GOLD_BRIGHT);

  draw_string(78, 150, "[ENTER] PLAY INFINITE MODE", PAL_WHITE);
}
