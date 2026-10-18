#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "pak.h"

/*
 * Display ownership and the per-frame compositor.
 *
 * Each frame the visible 320x236 window of the room image is composited over
 * the region's painted backdrop, which scrolls at half the camera speed
 * (parallax). Both images have one-pixel-shifted twins so every row the asm
 * compositor reads is word aligned. The tile layer runs at tile scale 0 (8x8
 * tiles at 640x480) above the pixel layer; tile 0 is fully transparent and
 * tile 1 solid black, used for the letterbox bars. The bottom bar also
 * covers pixel rows 236-239, which user mode cannot map.
 */

#define TILE_CLEAR 0
#define TILE_BLACK 1
#define HERO_SPRITE 0
#define SPRITE_SHORTS 1024

volatile short* g_pixel_fb;
unsigned short* g_back;          /* the frame being drawn; present() shows it */
volatile short* g_tile_fb;
volatile short* g_tilemap;
volatile short* g_spritemap;

static unsigned short* backdrop;      /* region backdrop, in the pak */
static unsigned short* backdrop_odd;  /* shifted left by one pixel */
static int backdrop_w;
static int backdrop_h;

/* A sprite sheet from the pak plus left-facing copies made at load. */
struct SheetCache {
  unsigned short* right;
  unsigned short* left;
  int count;
};

#define SHEET_SLOTS 4
static struct SheetCache sheets[4];
static int shown_sheet[16];
static int shown_frame[16];
static int shown_facing[16];

int display_init(void) {
  g_pixel_fb = get_pixel_fb();
  if ((int)g_pixel_fb == -1) {
    puts("retake: cannot map the pixel framebuffer (get_pixel_fb, trap 62, failed)\n");
    return -1;
  }
  /* Clear the terminal and hide its cursor, then let it act on that before
   * we take over the tile framebuffer. */
  puts("\x1b[2J\x1b[?25l");
  sleep(30);
  g_tilemap = get_tilemap();
  g_tile_fb = get_tile_fb();
  g_spritemap = get_spritemap();
  for (int s = 0; s < NUM_SPRITES; s++) set_sprite_coords(s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  set_pixel_scale(0);
  hud_init();
  g_back = (unsigned short*)mmap(SCREEN_W * VIEW_H * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  if ((int)g_back == -1) {
    puts("retake: out of memory for the back buffer\n");
    return -1;
  }
  return 0;
}

/* Give the display back in a state the terminal can recover from. */
void display_shutdown(void) {
  for (int s = 0; s < NUM_SPRITES; s++) set_sprite_coords(s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  for (int i = 0; i < FB_NUM_TILES; i++) g_tile_fb[i] = TILE_CLEAR;
}

/* Prepare the backdrop of region number `region_id`. */
static int shown_region = -1;

void render_region(int region_id) {
  if (region_id == shown_region) return;
  shown_region = region_id;
  struct PakRegion* r = pak_region_by_id(region_id);
  backdrop = (unsigned short*)(g_pak + r->backdrop_off);
  backdrop_w = (int)r->backdrop_w;
  backdrop_h = (int)r->backdrop_h;
  int total = backdrop_w * backdrop_h;
  if (backdrop_odd == 0) {
    backdrop_odd = (unsigned short*)mmap(ROOM_MAX_PW * ROOM_MAX_PH * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  }
  for (int i = 0; i < total - 1; i++) backdrop_odd[i] = backdrop[i + 1];
  backdrop_odd[total - 1] = backdrop[total - 1];
}

/* Composite the room and backdrop into the back buffer for a camera whose
 * top-left is (cam_x, cam_y) in room pixels (clamped inside the room by the
 * caller). Entities are drawn over it next, then present() copies the whole
 * frame out, so the display never catches a frame with entities missing. */
void render_frame(int cam_x, int cam_y) {
  unsigned short* room = (cam_x & 1) ? g_room_img_odd : g_room_img;
  int rx = cam_x & ~1;
  int bx = cam_x >> 1;
  int max_bx = backdrop_w - SCREEN_W;
  if (bx > max_bx) bx = max_bx;
  unsigned short* bg = (bx & 1) ? backdrop_odd : backdrop;
  bx &= ~1;
  int by = cam_y >> 1;
  int max_by = backdrop_h - VIEW_H;
  if (by > max_by) by = max_by;
  if (by < 0) by = 0;
  unsigned* opaque = (cam_x & 1) ? g_span_opaque_odd : g_span_opaque;
  unsigned* clear = (cam_x & 1) ? g_span_clear_odd : g_span_clear;
  unsigned short* room_row = room + cam_y * g_room_pw + rx;
  unsigned short* bg_row = bg + by * backdrop_w + bx;
  unsigned short* dst = g_back;
  int end_x = rx + SCREEN_W;
  for (int y = 0; y < VIEW_H; y++) {
    unsigned op = opaque[cam_y + y];
    unsigned cl = clear[cam_y + y];
    int px = rx;
    while (px < end_x) {
      int span = px >> 6;
      int span_end = (span + 1) << 6;
      if (span_end > end_x) span_end = end_x;
      int words = (span_end - px) >> 1;
      int off = px - rx;
      int whole = (px & 63) == 0 && span_end - px == 64;
      if (whole && ((op >> span) & 1)) {
        tl_copy_words((void*)(dst + off), (void*)(room_row + off), words);
      } else if (whole && ((cl >> span) & 1)) {
        tl_copy_words((void*)(dst + off), (void*)(bg_row + off), words);
      } else {
        rt_composite((void*)(dst + off), (void*)(room_row + off), (void*)(bg_row + off), words);
      }
      px = span_end;
    }
    dst += SCREEN_W;
    room_row += g_room_pw;
    bg_row += backdrop_w;
  }
}

/* Load sheet `index` from the pak and build its mirrored frames. */
void sheet_load(int index) {
  struct PakSprite* ps = pak_sprite(index);
  struct SheetCache* c = &sheets[index];
  c->right = (unsigned short*)(g_pak + ps->frames_off);
  c->count = (int)ps->frame_count;
  c->left = (unsigned short*)mmap(c->count * SPRITE_SHORTS * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  if ((int)c->left == -1) {
    puts("retake: out of memory mirroring a sprite sheet\n");
    exit(1);
  }
  for (int f = 0; f < c->count; f++) {
    unsigned short* src = c->right + f * SPRITE_SHORTS;
    unsigned short* dst = c->left + f * SPRITE_SHORTS;
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) dst[(y << 5) + x] = src[(y << 5) + 31 - x];
    }
  }
}

void sprites_init(void) {
  for (int s = 0; s < 16; s++) {
    shown_sheet[s] = -1;
    set_sprite_scale(s, 1);
  }
}

/* Show `frame` of `sheet` facing `facing` in hardware sprite `slot` with the
 * canvas top-left at screen (x, y). Pixels are uploaded only on change. */
void sprite_show(int slot, int sheet, int frame, int facing, int x, int y) {
  if (sheet != shown_sheet[slot] || frame != shown_frame[slot] || facing != shown_facing[slot]) {
    struct SheetCache* c = &sheets[sheet];
    unsigned short* src = (facing < 0 ? c->left : c->right) + frame * SPRITE_SHORTS;
    tl_copy_words((void*)(g_spritemap + slot * SPRITE_SHORTS), (void*)src, SPRITE_SHORTS >> 1);
    shown_sheet[slot] = sheet;
    shown_frame[slot] = frame;
    shown_facing[slot] = facing;
  }
  set_sprite_coords(slot, (unsigned)x, (unsigned)y);
}

void sprite_hide(int slot) {
  set_sprite_coords(slot, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
}

/* ---- software images (entities, props) ---- */

struct PakImage {
  unsigned w;
  unsigned h;
  unsigned off;
};

/* Draw image `id` with its top-left at screen (x, y), mirrored if `flip`.
 * Transparent pixels (0xF000) are skipped; clipped to the play area below
 * the HUD bar (pixel rows HUD_PIXEL_ROWS..VIEW_H-1). */
void draw_image(int id, int x, int y, int flip) {
  struct PakImage* im = (struct PakImage*)(g_pak + pak_header(PAKH_IMAGE_OFF)) + id;
  int w = (int)im->w;
  int h = (int)im->h;
  unsigned short* src = (unsigned short*)(g_pak + im->off);
  int y0 = y < HUD_PIXEL_ROWS ? HUD_PIXEL_ROWS : y;
  int y1 = y + h > VIEW_H ? VIEW_H : y + h;
  int x0 = x < 0 ? 0 : x;
  int x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
  if (x0 >= x1 || y0 >= y1) return;
  for (int py = y0; py < y1; py++) {
    unsigned short* row = src + (py - y) * w;
    unsigned short* dst = g_back + py * SCREEN_W;
    for (int px = x0; px < x1; px++) {
      int sx = px - x;
      unsigned short c = row[flip ? w - 1 - sx : sx];
      if (c != TRANSPARENT_PX) dst[px] = c;
    }
  }
}

int image_width(int id) {
  struct PakImage* im = (struct PakImage*)(g_pak + pak_header(PAKH_IMAGE_OFF)) + id;
  return (int)im->w;
}

int image_height(int id) {
  struct PakImage* im = (struct PakImage*)(g_pak + pak_header(PAKH_IMAGE_OFF)) + id;
  return (int)im->h;
}

/* A small bouncing arrow above an interactable, at screen (cx, y). */
void draw_prompt(int cx, int y, int clock) {
  int bob = (clock >> 3) & 1;
  for (int r = 0; r < 4; r++) {
    int py = y + r + bob;
    if (py < HUD_PIXEL_ROWS || py >= VIEW_H) continue;
    unsigned short* dst = g_back + py * SCREEN_W;
    for (int dx = -r; dx <= r; dx++) {
      int px = cx + dx;
      if (px >= 0 && px < SCREEN_W) dst[px] = 0x0EEF;
    }
  }
}

/* Copy the finished back buffer to the display in one pass. */
void present(void) {
  tl_copy_words((void*)g_pixel_fb, (void*)g_back, (SCREEN_W * VIEW_H) >> 1);
}
