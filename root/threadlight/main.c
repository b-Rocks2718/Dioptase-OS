#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/terminal.h"
#include "../crt/vga.h"

/*
 * THREADLIGHT render spike (temporary). Proves the hybrid display plan on the
 * emulator: the room lives in the hardware tile plane at tile scale 1 (an
 * 80x60-tile, 640x480 wrapping plane of 8x8 tiles in 320x240 screen space) and
 * the camera is just the tile scroll registers; the pixel layer behind it is a
 * horizontally wrapping parallax background redrawn every frame; the hero is
 * a scale-1 hardware sprite on top. Arrow keys move the hero, Esc quits.
 */

void tl_copy_words(void* dst, void* src, int words);

#define ROOM_W 80
#define ROOM_H 60
#define SCREEN_W 320
#define SCREEN_H 240
#define FB_ROWS 236
#define BG_W 640
#define KEY_ESC 0x1B
#define KEY_RIGHT 0x85
#define KEY_LEFT 0x86
#define KEY_DOWN 0x87
#define KEY_UP 0x88

static volatile short* tile_fb;
static volatile short* tilemap;
static volatile short* spritemap;
static volatile short* pixel_fb;
static unsigned short* bg;
static unsigned short* bg_odd;
static int key_down[256];
static int pending_code = -1;

/* Pack 4-bit channels into the display's 0x0BGR format. */
static int rgb(int r, int g, int b) {
  return (b << 8) | (g << 4) | r;
}

/* Upload the three spike tiles: 0 = air (transparent), 1 = bevelled stone,
 * 2 = green crystal. */
static void make_tiles(void) {
  for (int i = 0; i < 64; i++) tilemap[i] = (short)0xF000;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int c = rgb(5, 4, 6);
      if (y == 0 || x == 0) c = rgb(8, 7, 9);
      else if (y == 7 || x == 7) c = rgb(2, 2, 3);
      tilemap[64 + y * 8 + x] = (short)c;
      int d = x - 3;
      if (d < 0) d = -d;
      int inside = d <= (y >> 1) + 1 && y > 0;
      tilemap[128 + y * 8 + x] = (short)(inside ? rgb(4, 14 - y, 8) : 0xF000);
    }
  }
}

/* Fill the whole 80x60 tile plane with a graybox room. */
static void build_room(void) {
  for (int ty = 0; ty < ROOM_H; ty++) {
    for (int tx = 0; tx < ROOM_W; tx++) {
      int t = 0;
      if (tx < 2 || ty < 2 || tx >= ROOM_W - 2 || ty >= ROOM_H - 2) t = 1;
      if (ty == 50 && tx > 10 && tx < 30) t = 1;
      if (ty == 42 && tx > 34 && tx < 50) t = 1;
      if (ty == 30 && tx > 20 && tx < 70) t = 1;
      if (tx == 60 && ty > 10 && ty < 30) t = 1;
      if (ty == 29 && (tx == 30 || tx == 45 || tx == 55)) t = 2;
      tile_fb[ty * ROOM_W + tx] = (short)t;
    }
  }
}

/* Paint a 640-wide wrapping background (dusk gradient, distant pillars,
 * stars) plus a copy shifted left by one pixel, so any parallax offset is
 * word aligned in one of the two. */
static void build_background(void) {
  bg = (unsigned short*)mmap(BG_W * FB_ROWS * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  bg_odd = (unsigned short*)mmap(BG_W * FB_ROWS * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  unsigned seed = 12345;
  for (int y = 0; y < FB_ROWS; y++) {
    int band = y >> 4;
    int base = rgb(1 + (band >> 2), 1 + (band >> 1), 4 + (band >> 1));
    unsigned short* row = bg + y * BG_W;
    for (int x = 0; x < BG_W; x++) row[x] = (unsigned short)base;
  }
  for (int p = 0; p < 10; p++) {
    int x0 = p * 64 + 10;
    int top = 60 + ((p * 37) & 63);
    for (int y = top; y < FB_ROWS; y++) {
      unsigned short* row = bg + y * BG_W;
      for (int x = x0; x < x0 + 22; x++) row[x] = (unsigned short)rgb(2, 3, 6);
    }
  }
  for (int s = 0; s < 120; s++) {
    seed = seed * 1103515245 + 12345;
    int x = (seed >> 8) % BG_W;
    int y = (seed >> 20) % 120;
    bg[y * BG_W + x] = (unsigned short)rgb(12, 12, 15);
  }
  for (int y = 0; y < FB_ROWS; y++) {
    unsigned short* row = bg + y * BG_W;
    unsigned short* odd = bg_odd + y * BG_W;
    for (int x = 0; x < BG_W - 1; x++) odd[x] = row[x + 1];
    odd[BG_W - 1] = row[0];
  }
}

/* Copy the visible window of the background at half the camera speed. */
static void draw_background(int cam_x) {
  int px = (cam_x >> 1) % BG_W;
  unsigned short* src = bg;
  if (px & 1) {
    src = bg_odd;
    px -= 1;
  }
  int first = BG_W - px;
  if (first > SCREEN_W) first = SCREEN_W;
  for (int y = 0; y < FB_ROWS; y++) {
    unsigned short* row = src + y * BG_W;
    volatile short* dst = pixel_fb + y * SCREEN_W;
    tl_copy_words((void*)dst, (void*)(row + px), first >> 1);
    if (first < SCREEN_W) tl_copy_words((void*)(dst + first), (void*)row, (SCREEN_W - first) >> 1);
  }
}

/* Placeholder hero in sprite 0: a round body, two wings and a lantern. */
static void make_hero_sprite(void) {
  volatile short* s = spritemap;
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      int c = 0xF000;
      int dx = x - 16;
      int dy = y - 18;
      if (dx * dx + dy * dy < 30) c = rgb(13, 12, 10);
      else if ((dx - 6) * (dx - 6) + (dy + 3) * (dy + 3) < 20) c = rgb(9, 8, 12);
      else if ((dx + 6) * (dx + 6) + (dy + 3) * (dy + 3) < 20) c = rgb(9, 8, 12);
      else if ((dx - 9) * (dx - 9) + (dy - 4) * (dy - 4) < 6) c = rgb(6, 15, 9);
      s[y * 32 + x] = (short)c;
    }
  }
}

/* 3x5 digit glyphs, one bit per pixel, row-major from the top-left. */
static int digit_bits[10] = {
  0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
};

/* Draw a number into sprite 1 (top-left HUD corner) at 2x2 dots. */
static void show_number(int n) {
  volatile short* s = spritemap + 32 * 32;
  for (int i = 0; i < 32 * 32; i++) s[i] = (short)0xF000;
  int digits[4];
  for (int i = 3; i >= 0; i--) {
    digits[i] = n % 10;
    n = n / 10;
  }
  for (int d = 0; d < 4; d++) {
    int bits = digit_bits[digits[d]];
    for (int row = 0; row < 5; row++) {
      for (int col = 0; col < 3; col++) {
        if (bits & (1 << (14 - (row * 3 + col)))) {
          int x = d * 8 + col * 2;
          int y = row * 2;
          s[y * 32 + x] = (short)rgb(15, 15, 15);
          s[y * 32 + x + 1] = (short)rgb(15, 15, 15);
          s[(y + 1) * 32 + x] = (short)rgb(15, 15, 15);
          s[(y + 1) * 32 + x + 1] = (short)rgb(15, 15, 15);
        }
      }
    }
  }
}

/* Drain raw key records (keycode, then TERMINAL_RAW_RELEASE or 0). */
static void poll_input(void) {
  unsigned char buf[32];
  int avail = fd_bytes_available(STDIN);
  while (avail > 0) {
    int n = read(STDIN, buf, avail < 32 ? avail : 32);
    if (n <= 0) break;
    avail -= n;
    for (int i = 0; i < n; i++) {
      if (pending_code < 0) {
        pending_code = buf[i];
        continue;
      }
      key_down[pending_code] = buf[i] != TERMINAL_RAW_RELEASE;
      pending_code = -1;
    }
  }
}

int main(void) {
  pixel_fb = get_pixel_fb();
  if ((int)pixel_fb == -1) {
    puts("threadlight: cannot map the pixel framebuffer (get_pixel_fb failed)\n");
    return 1;
  }
  puts("\x1b[2J\x1b[?25l");
  sleep(30);
  tilemap = get_tilemap();
  tile_fb = get_tile_fb();
  spritemap = get_spritemap();
  for (int s = 0; s < 16; s++) set_sprite_coords(s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  set_tile_scale(1);
  make_tiles();
  build_room();
  build_background();
  make_hero_sprite();
  set_sprite_scale(0, 1);
  set_sprite_scale(1, 1);
  set_keyboard_raw(1);

  int hero_x = 100;
  int hero_y = 380;
  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int frames = 0;
  int auto_t = 0;
  while (!key_down[KEY_ESC]) {
    unsigned now = get_vga_frame_counter();
    while (now == last) now = get_vga_frame_counter();
    int ticks = (int)(now - last);
    last = now;
    poll_input();
    for (int t = 0; t < ticks; t++) {
      if (key_down[KEY_RIGHT]) hero_x += 2;
      if (key_down[KEY_LEFT]) hero_x -= 2;
      if (key_down[KEY_DOWN]) hero_y += 2;
      if (key_down[KEY_UP]) hero_y -= 2;
      if (key_down['a']) {
        auto_t++;
        hero_x = 160 + ((auto_t * 3) % 300);
        hero_y = 120 + ((auto_t * 2) % 240);
      }
    }
    if (hero_x < 16) hero_x = 16;
    if (hero_y < 16) hero_y = 16;
    if (hero_x > ROOM_W * 8 - 16) hero_x = ROOM_W * 8 - 16;
    if (hero_y > ROOM_H * 8 - 16) hero_y = ROOM_H * 8 - 16;
    int cam_x = hero_x - SCREEN_W / 2;
    int cam_y = hero_y - SCREEN_H / 2;
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    if (cam_x > ROOM_W * 8 - SCREEN_W) cam_x = ROOM_W * 8 - SCREEN_W;
    if (cam_y > ROOM_H * 8 - SCREEN_H) cam_y = ROOM_H * 8 - SCREEN_H;
    draw_background(cam_x);
    set_hscroll((unsigned)(-cam_x));
    set_vscroll((unsigned)(-cam_y));
    set_sprite_coords(0, (unsigned)(hero_x - cam_x - 16), (unsigned)(hero_y - cam_y - 16));
    frames++;
    if (now - fps_mark >= 60) {
      show_number(frames * 60 / (int)(now - fps_mark));
      set_sprite_coords(1, 4, 4);
      frames = 0;
      fps_mark = now;
    }
  }
  set_keyboard_raw(0);
  for (int s = 0; s < 16; s++) set_sprite_coords(s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  set_hscroll(0);
  set_vscroll(0);
  return 0;
}
