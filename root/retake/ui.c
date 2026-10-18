#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "pak.h"

/*
 * Text, intertitle cards and the HUD.
 *
 * Fonts come from the pak (tools/fontgen.py -> build.py): proportional
 * glyphs with ink levels 0..3, drawn into the pixel layer through a 4-entry
 * color ramp (level 0 is left untouched). Intertitle cards take over the
 * screen: the card frame image, centered wrapped lines, sprites hidden. The
 * HUD is a two-tile-row bar on the tile layer (tile scale 0), so its icons
 * are drawn at 640x480 density.
 */

struct PakFont {
  unsigned height;
  unsigned glyphs_off;
  unsigned pixels_off;
};

struct PakGlyph {
  unsigned char adv;
  unsigned char width;
  unsigned short pad;
  unsigned off;
};

#define HUD_ROW_TOP 0
#define HUD_ROW_BOTTOM 1

static struct PakFont* font_entry(int font) {
  return (struct PakFont*)(g_pak + pak_header(PAKH_FONT_OFF)) + font;
}

int font_height(int font) {
  return (int)font_entry(font)->height;
}

static struct PakGlyph* glyph(struct PakFont* f, int c) {
  if (c < 32 || c > 126) c = '?';
  return (struct PakGlyph*)(g_pak + f->glyphs_off) + (c - 32);
}

int text_width(int font, char* s) {
  struct PakFont* f = font_entry(font);
  int w = 0;
  while (*s && *s != '\n') {
    w += glyph(f, *s)->adv;
    s++;
  }
  return w;
}

/* Draw one line (up to '\n' or NUL) with its top-left at (x, y) in the pixel
 * framebuffer. ramp[1..3] are the colors for ink levels 1..3. Clips to the
 * user-visible framebuffer. Returns the pen x after the line. */
int draw_text(int x, int y, char* s, int font, short* ramp) {
  struct PakFont* f = font_entry(font);
  unsigned char* pixels = g_pak + f->pixels_off;
  int h = (int)f->height;
  while (*s && *s != '\n') {
    struct PakGlyph* gl = glyph(f, *s);
    unsigned char* src = pixels + gl->off;
    int w = gl->width;
    for (int gy = 0; gy < h; gy++) {
      int py = y + gy;
      if (py < 0 || py >= VIEW_H) {
        src += w;
        continue;
      }
      volatile short* row = g_pixel_fb + py * SCREEN_W;
      for (int gx = 0; gx < w; gx++) {
        int level = src[gx];
        int px = x + gx;
        if (level && px >= 0 && px < SCREEN_W) row[px] = ramp[level];
      }
      src += w;
    }
    x += gl->adv;
    s++;
  }
  return x;
}

/* Draw a line centered on screen column 160. */
void draw_text_centered(int y, char* s, int font, short* ramp) {
  draw_text((SCREEN_W - text_width(font, s)) >> 1, y, s, font, ramp);
}

/* ---- intertitle cards ---- */

static short card_ramp[4] = {0, 0x234, 0x9AB, 0xDEF};

static unsigned* script_entry(int script) {
  unsigned* offs = (unsigned*)(g_pak + pak_header(PAKH_SCRIPT_OFF));
  return (unsigned*)(g_pak + offs[script]);
}

static void draw_card(char* text) {
  tl_copy_words((void*)g_pixel_fb, (void*)(g_pak + pak_header(PAKH_CARD_OFF)), (SCREEN_W * VIEW_H) >> 1);
  int lines = 1;
  for (char* p = text; *p; p++) {
    if (*p == '\n' && p[1]) lines++;
  }
  int lh = font_height(FONT_SERIF) + 3;
  int y = (VIEW_H - lines * lh) >> 1;
  char* line = text;
  while (*line) {
    draw_text_centered(y, line, FONT_SERIF, card_ramp);
    while (*line && *line != '\n') line++;
    if (*line == '\n') line++;
    y += lh;
  }
}

/* Whether the player asked to advance a card this frame. */
static int advance_pressed(void) {
  int bits = input_take_pressed();
  if (bits & (IN_JUMP | IN_ATTACK | IN_INTERACT)) return 1;
  if (input_key_hit(KEY_ESC)) return 1;
  return 0;
}

/* Show every card of `script`, each until the player presses a button (after
 * a short beat, so a held key cannot skip a card unread). Takes over the
 * display; the caller redraws the game afterwards. */
void card_show(int script) {
  unsigned* entry = script_entry(script);
  int cards = (int)entry[0];
  for (int s = 0; s < NUM_SPRITES; s++) sprite_hide(s);
  hud_hide();
  for (int c = 0; c < cards; c++) {
    draw_card((char*)(g_pak + entry[1 + c]));
    unsigned start = get_vga_frame_counter();
    input_poll();
    input_take_pressed();
    while (1) {
      unsigned now = get_vga_frame_counter();
      input_poll();
      int go = advance_pressed();
      if (go && now - start > 20) break;
      sleep(1);
    }
  }
}

/* ---- HUD ---- */

static int hud_visible;

void hud_init(void) {
  short* tiles = (short*)(g_pak + pak_header(PAKH_HUD_TILES_OFF));
  int count = (int)pak_header(PAKH_HUD_TILE_COUNT);
  tl_copy_words((void*)g_tilemap, (void*)tiles, count << 5);
  hud_hide();
}

void hud_hide(void) {
  for (int i = 0; i < FB_NUM_TILES; i++) g_tile_fb[i] = HT_CLEAR;
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    g_tile_fb[x] = HT_BLACK;
    g_tile_fb[(TILE_COL_HEIGHT - 1) * TILE_ROW_WIDTH + x] = HT_BLACK;
  }
  hud_visible = 0;
}

static void hud_icon(int col, int first) {
  g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + col] = (short)first;
  g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + col + 1] = (short)(first + 1);
  g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + col] = (short)(first + 2);
  g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + col + 1] = (short)(first + 3);
}

static int hud_char_index(int c, char* set) {
  for (int i = 0; set[i]; i++) {
    if (set[i] == c) return i;
  }
  return 0;
}

/* Text on the HUD bar: each character is an 8x16 cell (two tiles). */
void hud_text(int col, char* s, int red) {
  while (*s) {
    int c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    int first = red ? HT_FONT_RED + (hud_char_index(c, "REC0123456789") << 1)
                    : HT_FONT + (hud_char_index(c, HUD_TEXT_CHARS) << 1);
    g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + col] = (short)first;
    g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + col] = (short)(first + 1);
    col++;
    s++;
  }
}

void hud_number(int col, int value, int digits, int red) {
  char buf[8];
  for (int i = digits - 1; i >= 0; i--) {
    buf[i] = (char)('0' + value % 10);
    value = value / 10;
  }
  buf[digits] = 0;
  hud_text(col, buf, red);
}

/* Draw the bar: health frames, scrip, take slots and the REC light. */
void hud_draw(int health, int max_health, int scrip, int slots, int slot_limit, int recording, int blink) {
  hud_visible = 1;
  int col = 2;
  for (int i = 0; i < max_health; i++) {
    hud_icon(col, i < health ? HT_FRAME_FULL : HT_FRAME_EMPTY);
    col += 2;
  }
  hud_icon(30, HT_COIN);
  hud_number(32, scrip, 4, 0);
  col = 46;
  for (int i = 0; i < slot_limit; i++) {
    hud_icon(col, (slots >> i) & 1 ? HT_REEL_FULL : HT_REEL_EMPTY);
    col += 2;
  }
  if (recording && blink) {
    hud_icon(56, HT_REC_ON);
    hud_text(58, "REC", 1);
  } else {
    g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + 56] = HT_BLACK;
    g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + 57] = HT_BLACK;
    g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + 56] = HT_BLACK;
    g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + 57] = HT_BLACK;
    hud_text(58, "   ", 0);
  }
}

/* Black out the whole HUD bar (both rows). */
void hud_bar_clear(void) {
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    g_tile_fb[HUD_ROW_TOP * TILE_ROW_WIDTH + x] = HT_BLACK;
    g_tile_fb[HUD_ROW_BOTTOM * TILE_ROW_WIDTH + x] = HT_BLACK;
  }
}
