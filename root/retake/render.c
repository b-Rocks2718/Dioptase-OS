#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"

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
volatile short* g_tile_fb;
volatile short* g_tilemap;
volatile short* g_spritemap;

static unsigned short* backdrop;      /* region backdrop, in the pak */
static unsigned short* backdrop_odd;  /* shifted left by one pixel */
static int backdrop_w;
static int backdrop_h;

static unsigned short* hero_frames;   /* right-facing frames, in the pak */
static unsigned short* hero_mirror;   /* left-facing copies */
static int hero_frame_count;
static int hero_shown_frame = -1;
static int hero_shown_facing;

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
  for (int i = 0; i < 64; i++) {
    g_tilemap[TILE_CLEAR * 64 + i] = (short)TRANSPARENT_PX;
    g_tilemap[TILE_BLACK * 64 + i] = 0;
  }
  for (int i = 0; i < FB_NUM_TILES; i++) g_tile_fb[i] = TILE_CLEAR;
  /* letterbox: the top tile row and the bottom one (which hides rows 236-239) */
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    g_tile_fb[x] = TILE_BLACK;
    g_tile_fb[(TILE_COL_HEIGHT - 1) * TILE_ROW_WIDTH + x] = TILE_BLACK;
  }
  return 0;
}

/* Give the display back in a state the terminal can recover from. */
void display_shutdown(void) {
  for (int s = 0; s < NUM_SPRITES; s++) set_sprite_coords(s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  for (int i = 0; i < FB_NUM_TILES; i++) g_tile_fb[i] = TILE_CLEAR;
}

/* Prepare region `region_index`'s backdrop (pak region entry index). */
void render_region(int region_index) {
  struct PakRegion* r = pak_region(region_index);
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

/* Composite the frame for a camera whose top-left is (cam_x, cam_y) in room
 * pixels. The caller clamps the camera inside the room. */
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
  unsigned short* room_row = room + cam_y * g_room_pw + rx;
  unsigned short* bg_row = bg + by * backdrop_w + bx;
  volatile short* dst = g_pixel_fb;
  for (int y = 0; y < VIEW_H; y++) {
    rt_composite((void*)dst, (void*)room_row, (void*)bg_row, SCREEN_W >> 1);
    dst += SCREEN_W;
    room_row += g_room_pw;
    bg_row += backdrop_w;
  }
}

void hero_sprite_init(void) {
  struct PakSprite* s = pak_sprite(0);
  hero_frames = (unsigned short*)(g_pak + s->frames_off);
  hero_frame_count = (int)s->frame_count;
  hero_mirror = (unsigned short*)mmap(hero_frame_count * SPRITE_SHORTS * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  for (int f = 0; f < hero_frame_count; f++) {
    unsigned short* src = hero_frames + f * SPRITE_SHORTS;
    unsigned short* dst = hero_mirror + f * SPRITE_SHORTS;
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) dst[(y << 5) + x] = src[(y << 5) + 31 - x];
    }
  }
  set_sprite_scale(HERO_SPRITE, 1);
}

/* Show frame `frame` facing `facing` with the canvas top-left at screen
 * (x, y); uploads pixels only when the frame or facing changed. */
void hero_sprite_place(int frame, int facing, int x, int y) {
  if (frame != hero_shown_frame || facing != hero_shown_facing) {
    unsigned short* src = (facing < 0 ? hero_mirror : hero_frames) + frame * SPRITE_SHORTS;
    tl_copy_words((void*)(g_spritemap + HERO_SPRITE * SPRITE_SHORTS), (void*)src, SPRITE_SHORTS >> 1);
    hero_shown_frame = frame;
    hero_shown_facing = facing;
  }
  set_sprite_coords(HERO_SPRITE, (unsigned)x, (unsigned)y);
}
