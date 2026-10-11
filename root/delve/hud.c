#include "delve.h"

/*
 * Text and 2D overlays: the 3x5 HUD font and the OS's own 8x8 font (decoded
 * from common/text_tiles.s by the asset generator) drawn either into the 3D
 * view's back buffer or into the status bar.
 *
 * The status bar is kept as a linear 160x24 buffer and uploaded to its five
 * hardware sprites only when something on it changed.
 */

unsigned g_hud_words[1920];
static bool hud_dirty = true;

/* Address of view pixel (x, y) in the tile-ordered back buffer. */
static unsigned short* view_pixel(int x, int y){
  return g_view + (x >> 3) * VIEW_COL_GROUP_SHORTS + (x & 7) + y * VIEW_ROW_SHORTS;
}

/* Plot one pixel on a surface, clipped to its bounds. */
static void plot(int surface, int x, int y, unsigned short color){
  if (surface == SURF_VIEW){
    if (x < 0 || y < 0 || x >= VIEW_W || y >= VIEW_H) return;
    *view_pixel(x, y) = color;
  } else {
    if (x < 0 || y < 0 || x >= HUD_W || y >= HUD_H) return;
    g_hud[y * HUD_W + x] = color;
    hud_dirty = true;
  }
}

/* Width in pixels of `s` in the 3x5 font (4 pixels per glyph advance). */
int text_width(char* s){
  int n = 0;
  while (s[n]) n += 1;
  return n > 0 ? n * 4 - 1 : 0;
}

/* Draw `s` in the 3x5 font with its top-left corner at (x, y). Lowercase
 * letters are shown as capitals. Returns the x just past the text. */
int draw_text(int surface, int x, int y, char* s, unsigned short color){
  for (; *s; ++s){
    int c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 95) c = '?';
    unsigned char* rows = font_small + (c - 32) * 5;
    for (int r = 0; r < 5; ++r){
      int bits = rows[r];
      if (bits & 4) plot(surface, x, y + r, color);
      if (bits & 2) plot(surface, x + 1, y + r, color);
      if (bits & 1) plot(surface, x + 2, y + r, color);
    }
    x += 4;
  }
  return x;
}

/* Draw text with a one-pixel dark drop shadow for legibility over the view. */
int draw_text_shadow(int surface, int x, int y, char* s, unsigned short color){
  draw_text(surface, x + 1, y + 1, s, 0x0000);
  return draw_text(surface, x, y, s, color);
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

/* Draw `s` in the OS 8x8 font, proportionally spaced, scaled by `scale`. */
int draw_big_text(int surface, int x, int y, char* s, unsigned short color, int scale){
  for (; *s; ++s){
    int c = *s;
    if (c < 32 || c > 127) c = '?';
    int left = font_big_spans[(c - 32) * 2];
    int width = font_big_spans[(c - 32) * 2 + 1];
    unsigned char* rows = font_big + (c - 32) * 8;
    for (int r = 0; r < 8; ++r){
      int bits = rows[r];
      for (int col = 0; col < width; ++col){
        if (bits & (0x80 >> (left + col))){
          for (int sy = 0; sy < scale; ++sy){
            for (int sx = 0; sx < scale; ++sx){
              plot(surface, x + col * scale + sx, y + r * scale + sy, color);
            }
          }
        }
      }
    }
    x += (width + 1) * scale;
  }
  return x;
}

/* Format a non-negative integer into `buf` (at least 12 bytes). */
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

/* Fill a rectangle on a surface. */
void fill_rect(int surface, int x, int y, int w, int h, unsigned short color){
  for (int yy = y; yy < y + h; ++yy){
    for (int xx = x; xx < x + w; ++xx){
      plot(surface, xx, yy, color);
    }
  }
}

/* Mark the status bar as needing an upload (after direct g_hud writes). */
void hud_touch(void){
  hud_dirty = true;
}

/* Copy the status bar into its five hardware sprites if it changed. */
void hud_upload(void){
  if (!hud_dirty) return;
  for (int s = 0; s < HUD_SPRITE_COUNT; ++s){
    volatile short* dst = g_spritemap + (HUD_SPRITE_FIRST + s) * SPRITE_SHORTS;
    unsigned short* src = g_hud + s * 32;
    for (int y = 0; y < HUD_H; ++y){
      copy_words((void*)(dst + y * 32), src + y * HUD_W, 16);
    }
  }
  hud_dirty = false;
}
