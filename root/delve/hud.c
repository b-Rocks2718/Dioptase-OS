#include "delve.h"

/*
 * 2D drawing: the back-buffer and status-bar surfaces, the status bar
 * upload, and the tile layer's overlays (text and banners). Text uses the
 * OS's own 8x8 font, decoded from common/text_tiles.s by the asset generator.
 *
 * The status bar is a linear 320x40 buffer copied into framebuffer rows
 * 192..231 only when something on it changed.
 *
 * Tile overlays are immediate mode: every frame starts with text_begin(),
 * places the text and banners it wants, and text_end() writes only the tile
 * framebuffer entries that differ from what is on screen. Banners (headings
 * and the title logo) are rendered once into overlay tiles 128..255, so big
 * text costs nothing per frame and never touches the 3D view.
 */

unsigned g_hud_words[6400];
static bool hud_dirty = true;

/* Back-buffer offsets in shorts: pixel (x, y) is
 * g_view[view_row_off[y] + view_col_off[x]] (delve.h, "Screen layout"). */
int view_row_off[192];
int view_col_off[320];

/* Fill the back-buffer offset tables. */
void hud_init(void){
  for (int y = 0; y < VIEW_H; ++y){
    int half = y >= VIEW_HALF_H;
    view_row_off[y] = half * HALF_SHORTS + (y - half * VIEW_HALF_H) * 8;
  }
  for (int x = 0; x < VIEW_W; ++x){
    view_col_off[x] = (x >> 3) * CHUNK_SHORTS + (x & 7);
  }
}

/* Width of `s` in the proportional 8x8 font. */
int big_text_width(char* s){
  int w = 0;
  for (; *s; ++s){
    int c = *s;
    if (c < 32 || c > 127) c = '?';
    w += font_big_spans[(c - 32) * 2 + 1] + 1;
  }
  return w > 0 ? w - 1 : 0;
}

/* Format an integer into `buf` (at least 12 bytes). */
char* format_int(char* buf, int value){
  char tmp[12];
  int n = 0;
  bool negative = value < 0;
  unsigned v = negative ? -value : value;
  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while (v != 0);
  int i = 0;
  if (negative) buf[i++] = '-';
  while (n > 0) buf[i++] = tmp[--n];
  buf[i] = 0;
  return buf;
}

/* Fill a rectangle on a surface, clipped to its bounds. */
void fill_rect(int surface, int x, int y, int w, int h, unsigned short color){
  int x1 = x + w;
  int y1 = y + h;
  int max_w = surface == SURF_VIEW ? VIEW_W : HUD_W;
  int max_h = surface == SURF_VIEW ? VIEW_H : HUD_H;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > max_w) x1 = max_w;
  if (y1 > max_h) y1 = max_h;
  for (int yy = y; yy < y1; ++yy){
    if (surface == SURF_VIEW){
      unsigned short* row = g_view + view_row_off[yy];
      for (int xx = x; xx < x1; ++xx) row[view_col_off[xx]] = color;
    } else {
      unsigned short* row = g_hud + yy * HUD_W;
      for (int xx = x; xx < x1; ++xx) row[xx] = color;
    }
  }
  if (surface == SURF_HUD) hud_dirty = true;
}

/* Mark the status bar as needing an upload (after direct g_hud writes). */
void hud_touch(void){
  hud_dirty = true;
}

/* Copy the status bar into framebuffer rows HUD_Y.. if it changed. */
void hud_upload(void){
  if (!hud_dirty) return;
  copy_words((void*)(g_pixel_fb + HUD_Y * FB_W), g_hud, HUD_PIXELS / 2);
  hud_dirty = false;
}

/* ---- Tile layer text ---- */

/* The text rows (0..TEXT_BORDER_ROW - 1) as this frame wants them and as
 * they are on screen, TEXT_COLS entries per row, two entries per word. */
#define TEXT_ENTRIES 1160           /* TEXT_COLS * TEXT_BORDER_ROW */
static unsigned text_want[580];
static unsigned text_shown[580];

/* Build the overlay tiles (see delve.h, "Tile layer"), clear the visible
 * tile framebuffer, and lay the opaque border over framebuffer rows 232..239,
 * which user programs cannot draw (root/crt/sys.h, get_pixel_fb). */
void tiles_init(void){
  for (int t = 0; t < 128; ++t){
    volatile short* tile = g_tilemap + t * TILEMAP_SHORTS_PER_TILE;
    for (int y = 0; y < 8; ++y){
      int bits = 0;
      int above = 0;
      if (t >= 32){
        bits = font_big[(t - 32) * 8 + y];
        above = y > 0 ? font_big[(t - 32) * 8 + y - 1] : 0;
      }
      for (int x = 0; x < 8; ++x){
        int color = TRANSPARENT_PIXEL;
        if (t == TILE_BLACK){
          color = 0x0000;
        } else if (t == TILE_SOLID){
          color = 0xC000;
        } else if (t == TILE_BORDER){
          color = y == 0 ? 0x0136 : 0x0000;
        } else if (t > TILE_BAR && t < TILE_BAR + 8){
          color = x < t - TILE_BAR ? 0xC000 : 0x0000;
        } else if (bits & (0x80 >> x)){
          color = 0xC000;               /* glyph: drawn in the entry's color */
        } else if (x > 0 && (above & (0x80 >> (x - 1)))){
          color = 0x0000;               /* drop shadow, down and right */
        }
        tile[y * 8 + x] = color;
      }
    }
  }
  for (int i = 0; i < TILE_FB_COLS * 60; ++i){
    g_tile_fb[i] = TILE_CLEAR;
  }
  for (int col = 0; col < TEXT_COLS; ++col){
    g_tile_fb[TEXT_BORDER_ROW * TILE_FB_COLS + col] = TILE_BORDER;
  }
  for (int i = 0; i < TEXT_ENTRIES / 2; ++i){
    text_shown[i] = 0;
  }
}

/* Start a frame's tile overlays: nothing is wanted until placed again. */
void text_begin(void){
  for (int i = 0; i < TEXT_ENTRIES / 2; ++i){
    text_want[i] = 0;
  }
}

/* Place one tile entry for this frame (clipped to the text rows). */
static void text_put(int col, int row, int entry){
  if (col < 0 || row < 0 || col >= TEXT_COLS || row >= TEXT_BORDER_ROW) return;
  unsigned short* want = (unsigned short*)text_want;
  want[row * TEXT_COLS + col] = entry;
}

/* Write `s` from tile (col, row) in RGB332 `color`; spaces stay clear. */
void tile_text(int col, int row, char* s, int color){
  for (; *s; ++s, ++col){
    int c = *s;
    if (c < 32 || c > 127) c = '?';
    if (c != ' ') text_put(col, row, (TILE_GLYPH + c) | (color << 8));
  }
}

/* Write `s` centered on tile row `row`. */
void tile_text_centered(int row, char* s, int color){
  int n = 0;
  while (s[n]) n += 1;
  tile_text((TEXT_COLS - n) / 2, row, s, color);
}

/* Write `s` so that it ends just before tile column `end`. */
void tile_text_right(int end, int row, char* s, int color){
  int n = 0;
  while (s[n]) n += 1;
  tile_text(end - n, row, s, color);
}

/* A horizontal bar from tile (col, row): `width_px` pixels (a multiple of 8)
 * of which the first `filled_px` are solid `color` and the rest black. */
void tile_bar(int col, int row, int width_px, int filled_px, int color){
  for (int x = 0; x < width_px; x += 8, ++col){
    int n = filled_px - x;
    int tile = TILE_BLACK;
    if (n >= 8) tile = TILE_SOLID;
    else if (n > 0) tile = TILE_BAR + n;
    text_put(col, row, tile | (color << 8));
  }
}

/* Show this frame's tile overlays: write the entries that changed. */
void text_end(void){
  for (int row = 0; row < TEXT_BORDER_ROW; ++row){
    int base = row * (TEXT_COLS / 2);
    for (int k = 0; k < TEXT_COLS / 2; ++k){
      unsigned w = text_want[base + k];
      if (w == text_shown[base + k]) continue;
      text_shown[base + k] = w;
      volatile unsigned* dst = (volatile unsigned*)(g_tile_fb + row * TILE_FB_COLS + k * 2);
      *dst = w;
    }
  }
}

/* ---- Banners ---- */

/* A banner is big text drawn into a byte mask once, then colored into a
 * block of overlay tiles. Slot 0 starts at BANNER_TILE0 and slot 1 follows
 * it; a banner that would run past tile 255 is not shown. */
#define BANNER_TILE0 128
#define BANNER_MAX_W 256
#define BANNER_MAX_H 48
#define MASK_CLEAR 0
#define MASK_SHADOW 1               /* black drop shadow */
#define MASK_EDGE 2                 /* the logo's dark outline */
#define MASK_GLYPH 3

struct Banner {
  char text[12];
  int scale;
  bool logo;                        /* gradient fill, outline, and glint */
  unsigned short color;             /* fill of a plain banner (12-bit) */
  int width;                        /* pixels */
  int height;
  int cols;                         /* tiles; 0 when not shown */
  int rows;
  int tile0;
  int glint;                        /* logo: glint position in banner pixels */
  unsigned char mask[12288];        /* BANNER_MAX_W * BANNER_MAX_H */
};
static struct Banner banners[2];

/* The logo's fill, top to bottom: pale mint to deep emerald (0x0BGR). */
static unsigned short logo_ramp[8] = {
  0x0EFC, 0x0BFA, 0x08F7, 0x06E5, 0x05C3, 0x04A2, 0x0381, 0x0260
};

/* Raise every mask pixel of a scale x scale block to at least `value` (so
 * glyphs win over outlines, and outlines over shadows). */
static void mask_block(struct Banner* b, int x, int y, int value){
  for (int sy = 0; sy < b->scale; ++sy){
    int py = y + sy;
    if (py < 0 || py >= b->height) continue;
    for (int sx = 0; sx < b->scale; ++sx){
      int px = x + sx;
      if (px < 0 || px >= b->width) continue;
      unsigned char* m = &b->mask[py * BANNER_MAX_W + px];
      if (*m < value) *m = value;
    }
  }
}

/* Color of banner pixel (x, y) from its mask value. */
static unsigned short banner_color(struct Banner* b, int x, int y){
  int m = b->mask[y * BANNER_MAX_W + x];
  if (m == MASK_CLEAR) return TRANSPARENT_PIXEL;
  if (m == MASK_SHADOW) return 0x0000;
  if (m == MASK_EDGE) return 0x0120;
  if (!b->logo) return b->color;
  int gy = y - 1;                   /* the logo's text starts one pixel in */
  int d = x - b->glint + (gy >> 1);
  if (d >= 0 && d < 3) return 0x0FFF;
  int band = (gy * 8) / (8 * b->scale);
  if (band < 0) band = 0;
  if (band > 7) band = 7;
  return logo_ramp[band];
}

/* Copy tile columns [tc0, tc1) of banner `b` from its mask into its tiles. */
static void banner_paint(struct Banner* b, int tc0, int tc1){
  if (tc0 < 0) tc0 = 0;
  if (tc1 > b->cols) tc1 = b->cols;
  for (int tr = 0; tr < b->rows; ++tr){
    for (int tc = tc0; tc < tc1; ++tc){
      volatile short* tile = g_tilemap + (b->tile0 + tr * b->cols + tc) * TILEMAP_SHORTS_PER_TILE;
      for (int y = 0; y < 8; ++y){
        int py = tr * 8 + y;
        for (int x = 0; x < 8; ++x){
          int px = tc * 8 + x;
          tile[y * 8 + x] = (px < b->width && py < b->height)
                            ? banner_color(b, px, py) : TRANSPARENT_PIXEL;
        }
      }
    }
  }
}

/* Show `text` in banner slot `slot` at `scale` (an 8x8 glyph becomes
 * 8 * scale pixels), as the gradient logo or in plain `color` with a drop
 * shadow. Redraws the tiles only when something changed. Changing slot 0
 * moves slot 1's tiles, so set slot 0 first. */
void banner_set(int slot, char* text, int scale, bool logo, unsigned short color){
  struct Banner* b = &banners[slot];
  int tile0 = BANNER_TILE0;
  if (slot == 1 && banners[0].cols > 0){
    tile0 = banners[0].tile0 + banners[0].cols * banners[0].rows;
  }
  int n = 0;
  bool same = b->scale == scale && b->logo == logo && b->color == color && b->tile0 == tile0;
  while (text[n] && n < 11){
    if (b->text[n] != text[n]) same = false;
    n += 1;
  }
  if (b->text[n] != 0) same = false;
  if (same && b->cols > 0) return;
  for (int i = 0; i < n; ++i) b->text[i] = text[i];
  b->text[n] = 0;
  b->scale = scale;
  b->logo = logo;
  b->color = color;
  b->tile0 = tile0;
  b->width = big_text_width(b->text) * scale + (logo ? 3 : 1);
  b->height = 8 * scale + (logo ? 3 : 1);
  if (b->width > BANNER_MAX_W) b->width = BANNER_MAX_W;
  if (b->height > BANNER_MAX_H) b->height = BANNER_MAX_H;
  b->cols = (b->width + 7) >> 3;
  b->rows = (b->height + 7) >> 3;
  if (b->tile0 + b->cols * b->rows > 256){
    b->cols = 0;
    return;
  }
  for (int i = 0; i < BANNER_MAX_W * BANNER_MAX_H; ++i) b->mask[i] = MASK_CLEAR;
  int origin = logo ? 1 : 0;
  int x = origin;
  for (char* s = b->text; *s; ++s){
    int c = *s;
    if (c < 32 || c > 127) c = '?';
    int left = font_big_spans[(c - 32) * 2];
    int width = font_big_spans[(c - 32) * 2 + 1];
    unsigned char* rows = font_big + (c - 32) * 8;
    for (int r = 0; r < 8; ++r){
      for (int col = 0; col < width; ++col){
        if (!(rows[r] & (0x80 >> (left + col)))) continue;
        int px = x + col * scale;
        int py = origin + r * scale;
        if (logo){
          /* outline all around, black toward the lower right */
          for (int oy = -1; oy <= 2; ++oy){
            for (int ox = -1; ox <= 2; ++ox){
              mask_block(b, px + ox, py + oy, (ox == 2 || oy == 2) ? MASK_SHADOW : MASK_EDGE);
            }
          }
        } else {
          mask_block(b, px + 1, py + 1, MASK_SHADOW);
        }
        mask_block(b, px, py, MASK_GLYPH);
      }
    }
    x += (width + 1) * scale;
  }
  b->glint = -1000;
  banner_paint(b, 0, b->cols);
}

/* Hide banner slot `slot` (its tiles may be reused by the other slot). */
void banner_clear(int slot){
  banners[slot].cols = 0;
  banners[slot].text[0] = 0;
}

/* Move the logo's glint to `glint` (banner pixels), repainting only the
 * tile columns the old and new glint bands touch. */
void banner_glint(int slot, int glint){
  struct Banner* b = &banners[slot];
  if (!b->logo || b->cols == 0 || glint == b->glint) return;
  int old = b->glint;
  b->glint = glint;
  /* a glint at g lights x in [g - (y - 1) / 2, g - (y - 1) / 2 + 3) */
  int reach = b->height >> 1;
  if (old > -1000) banner_paint(b, (old - reach - 1) >> 3, ((old + 3) >> 3) + 1);
  banner_paint(b, (glint - reach - 1) >> 3, ((glint + 3) >> 3) + 1);
}

/* Width of banner `slot` in banner pixels (for sweeping its glint). */
int banner_width(int slot){
  return banners[slot].width;
}

/* Place banner `slot` this frame, centered, with its top on tile row `row`. */
void banner_place(int slot, int row){
  struct Banner* b = &banners[slot];
  if (b->cols == 0) return;
  int col0 = (TEXT_COLS - b->cols) / 2;
  for (int tr = 0; tr < b->rows; ++tr){
    for (int tc = 0; tc < b->cols; ++tc){
      text_put(col0 + tc, row + tr, b->tile0 + tr * b->cols + tc);
    }
  }
}
