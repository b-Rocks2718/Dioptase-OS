#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "pak.h"

/*
 * Display ownership and the hardware-scrolled view.
 *
 * The pixel layer is a 320x240 ring buffer (docs/mem_map.md): world pixel
 * (x, y) of the current room lives at framebuffer ((x mod 320), (y mod 240))
 * and the pixel scroll registers (set_pixel_scroll, trap 65) put the camera's
 * corner at the screen's top-left. Each room image is composed once with the
 * region backdrop behind it, so the background is a plain copy. Per frame:
 *   view_begin  sets the scroll, restores the background (into the g_back
 *               canvas) under last frame's drawings and under the strip the
 *               camera just uncovered;
 *   drawing     every call marks its rectangle with view_touch first, which
 *               restores any 8x8 world tile it touches for the first time;
 *   view_end    copies only touched tiles to the framebuffer, clipped to the
 *               camera window. Rows 236..239 go through write_pixel_tail
 *               (trap 66): user code cannot map them.
 * The tile layer runs at tile scale 0 (8x8 tiles at 640x480) above the pixel
 * layer. Black tile bars hide the view's edges: the HUD covers pixel rows
 * 0..7, a bottom bar rows 236..239, and side bars columns 0..7 and 312..319.
 * They are what let the scroll move up to 8 px a frame (4 px down) before
 * the uncovered strip is drawn without the stale strip ever showing.
 */

#define TILE_CLEAR 0
#define TILE_BLACK 1
#define HERO_SPRITE 0
#define SPRITE_SHORTS 1024

volatile short* g_pixel_fb;
volatile short* g_canvas;      /* where text is drawn: the display, or g_back while a card is composed */
unsigned short* g_back;          /* the play canvas (see view_begin), or a card being composed */
volatile short* g_tile_fb;
volatile short* g_tilemap;
volatile short* g_spritemap;

static unsigned short* backdrop;      /* region backdrop, in the pak */
static int backdrop_w;
static int backdrop_h;
#define BACKDROP_MAX_H 512
static int backdrop_row[BACKDROP_MAX_H];   /* pixel offset of each backdrop row */

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
  g_canvas = g_pixel_fb;
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
  g_back = (unsigned short*)mmap(CANVAS_W * CANVAS_H * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
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

/* ---- the region backdrop ---- */

static int shown_region = -1;

/* Select region `region_id`'s painted backdrop. It is painted into each room
 * image behind the room's transparent pixels, fixed to the room: a parallax
 * layer would have to be redrawn every frame, and the hardware-scrolled view
 * exists to redraw almost nothing. */
void render_region(int region_id) {
  if (region_id == shown_region) return;
  shown_region = region_id;
  struct PakRegion* r = pak_region_by_id(region_id);
  backdrop = (unsigned short*)(g_pak + r->backdrop_off);
  backdrop_w = (int)r->backdrop_w;
  backdrop_h = (int)r->backdrop_h;
  if (backdrop_h > BACKDROP_MAX_H) backdrop_h = BACKDROP_MAX_H;
  int offset = 0;
  for (int y = 0; y < backdrop_h; y++) {   /* row offsets without a software multiply */
    backdrop_row[y] = offset;
    offset += backdrop_w;
  }
}

/* Paint the backdrop into every transparent pixel of the room image, one
 * tile row (8 pixels) at a time with the asm compositor, walking the image
 * in its own column-of-tiles order. The backdrop's bottom edge lines up
 * with the room's floor; taller rooms repeat its top row, wider rooms
 * repeat it (backdrop widths are multiples of 8, so a tile never straddles
 * the seam). */
static void compose_backdrop(void) {
  int shift = backdrop_h - g_room_ph;
  unsigned short* tile = g_room_img;
  int bx = 0;
  for (int tx = 0; tx < g_room.w; tx++) {
    for (int ty = 0; ty < g_room.h; ty++) {
      for (int r = 0; r < 8; r++) {
        int by = (ty << 3) + r + shift;
        if (by < 0) by = 0;
        if (by >= backdrop_h) by = backdrop_h - 1;
        rt_composite((void*)tile, (void*)tile, (void*)(backdrop + backdrop_row[by] + bx), 4);
        tile += 8;
      }
    }
    bx += 8;
    if (bx >= backdrop_w) bx -= backdrop_w;
  }
}

/* ---- the hardware-scrolled view ---- */

#define TILES_X 42             /* CANVAS_W / 8 */
#define TILES_Y 32             /* CANVAS_H / 8 */
#define RING_TX 40             /* the ring buffer in 8x8 tiles */
#define RING_TY 30
#define TAIL_ROW 236           /* rows from here (pixels from 256 on row 236) are kernel-only */
#define TAIL_X 256

int g_row[240];
static int canvas_row[256];          /* r * CANVAS_W */
static int tile_index_row[32];       /* ty * TILES_X */
static unsigned char tile_state[1344];   /* per canvas tile: 1 background restored, 2 drawn on */
static short drawn_tx[2][1344];      /* world tiles drawn on: last frame's list and this frame's */
static short drawn_ty[2][1344];
static int drawn_count[2];
static int drawn_cur;
static int view_valid;               /* the framebuffer holds the window at (shown_x, shown_y) */
static int composed_serial = -1;     /* room image already has its backdrop */
static int shown_x;
static int shown_y;
static int scroll_x = -1;            /* values last written to the scroll registers */
static int scroll_y = -1;
static int cam_x0;                   /* this frame's camera and canvas origin */
static int cam_y0;
static int base_tx;                  /* world tile at canvas tile (0, 0) */
static int base_ty;
static int canvas_ox;
static int canvas_oy;
static unsigned short tail_rows[1280];   /* staging for framebuffer rows 236..239 */
static int tail_lo[4];
static int tail_hi[4];

/* The next view_begin redraws everything (a card or menu used the screen). */
void view_invalidate(void) {
  view_valid = 0;
}

/* Fill canvas tile (ctx, cty) with the background: the room image at its
 * world tile, or black outside the room. */
static void restore_tile(int ctx, int cty) {
  int wtx = base_tx + ctx;
  int wty = base_ty + cty;
  unsigned* dst = (unsigned*)(g_back + canvas_row[cty << 3] + (ctx << 3));
  int stride = CANVAS_W >> 1;
  if (wtx < 0 || wty < 0 || wtx >= g_room.w || wty >= g_room.h) {
    for (int r = 0; r < 8; r++) {
      dst[0] = 0;
      dst[1] = 0;
      dst[2] = 0;
      dst[3] = 0;
      dst += stride;
    }
    return;
  }
  unsigned* src = (unsigned*)(g_room_img + g_room_col_base[wtx] + (wty << 6));
  for (int r = 0; r < 8; r++) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    dst += stride;
    src += 4;
  }
}

/* Restore every canvas tile under screen rectangle [x0,x1) x [y0,y1) that
 * has not been restored this frame (no drawing mark). */
static void restore_rect(int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y1 > 240) y1 = 240;
  if (x0 >= x1 || y0 >= y1) return;
  int cx1 = (x1 - 1 + canvas_ox) >> 3;
  int cy1 = (y1 - 1 + canvas_oy) >> 3;
  for (int cty = (y0 + canvas_oy) >> 3; cty <= cy1; cty++) {
    unsigned char* st = tile_state + tile_index_row[cty];
    for (int ctx = (x0 + canvas_ox) >> 3; ctx <= cx1; ctx++) {
      if (st[ctx] & 1) continue;
      restore_tile(ctx, cty);
      st[ctx] |= 1;
    }
  }
}

/* Mark screen rectangle [x0,x1) x [y0,y1) as drawn on this frame: restore
 * the background under tiles not yet restored, and remember each tile so
 * the next frame erases it. Call before writing any pixels into g_back. */
void view_touch(int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < HUD_PIXEL_ROWS) y0 = HUD_PIXEL_ROWS;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y1 > VIEW_H) y1 = VIEW_H;
  if (x0 >= x1 || y0 >= y1) return;
  int cx1 = (x1 - 1 + canvas_ox) >> 3;
  int cy1 = (y1 - 1 + canvas_oy) >> 3;
  int list = drawn_cur;
  for (int cty = (y0 + canvas_oy) >> 3; cty <= cy1; cty++) {
    unsigned char* st = tile_state + tile_index_row[cty];
    for (int ctx = (x0 + canvas_ox) >> 3; ctx <= cx1; ctx++) {
      int s = st[ctx];
      if (s == 3) continue;
      if (!(s & 1)) restore_tile(ctx, cty);
      if (!(s & 2)) {
        int n = drawn_count[list];
        drawn_tx[list][n] = (short)(base_tx + ctx);
        drawn_ty[list][n] = (short)(base_ty + cty);
        drawn_count[list] = n + 1;
      }
      st[ctx] = 3;
    }
  }
}

/* Start a frame with the camera's top-left at room pixel (cam_x, cam_y);
 * cam_x may be down to -8 (the side bars), cam_y >= 0. Scrolls the display there and restores the background under
 * last frame's drawings and under any newly uncovered strip; if the camera
 * jumped further than the edge bars hide (or the view is invalid), restores
 * the whole window instead. */
void view_begin(int cam_x, int cam_y) {
  cam_x0 = cam_x;
  cam_y0 = cam_y;
  /* cam_x can be as low as -8; shift through a positive offset so the tile
   * origin floors correctly whatever >> does with negative values. */
  canvas_ox = (cam_x + 64) & 7;
  canvas_oy = cam_y & 7;
  base_tx = ((cam_x + 64) >> 3) - 8;
  base_ty = cam_y >> 3;
  if (canvas_row[1] == 0) {
    for (int r = 0; r < 256; r++) canvas_row[r] = r * CANVAS_W;
    for (int t = 0; t < 32; t++) tile_index_row[t] = t * TILES_X;
  }
  for (int y = 0; y < 240; y++) g_row[y] = canvas_row[y + canvas_oy] + canvas_ox;
  unsigned* st = (unsigned*)tile_state;
  for (int i = 0; i < 336; i++) st[i] = 0;
  drawn_cur ^= 1;
  drawn_count[drawn_cur] = 0;
  if (composed_serial != g_room_serial) {
    compose_backdrop();
    composed_serial = g_room_serial;
    view_valid = 0;
  }
  int sx = cam_x;
  while (sx >= 320) sx -= 320;
  while (sx < 0) sx += 320;          /* the camera may sit up to 8 px left of the room */
  int sy = cam_y;
  while (sy >= 240) sy -= 240;
  while (sy < 0) sy += 240;
  sx = sx ? 320 - sx : 0;          /* the scroll that shows (cam_x, cam_y) at the corner */
  sy = sy ? 240 - sy : 0;
  if (sx != scroll_x || sy != scroll_y) {
    set_pixel_scroll(sx, sy);
    scroll_x = sx;
    scroll_y = sy;
  }
  int dx = cam_x - shown_x;
  int dy = cam_y - shown_y;
  if (!view_valid || dx > 8 || dx < -8 || dy > 4 || dy < -8) {
    restore_rect(0, 0, SCREEN_W, 240);
    return;
  }
  if (dx > 0) restore_rect(SCREEN_W - dx, 0, SCREEN_W, 240);
  if (dx < 0) restore_rect(0, 0, -dx, 240);
  if (dy > 0) restore_rect(0, 240 - dy, SCREEN_W, 240);
  if (dy < 0) restore_rect(0, 0, SCREEN_W, -dy);
  int last = drawn_cur ^ 1;
  for (int i = 0; i < drawn_count[last]; i++) {
    int ctx = drawn_tx[last][i] - base_tx;
    int cty = drawn_ty[last][i] - base_ty;
    if (ctx < 0 || cty < 0 || ctx >= TILES_X || cty >= TILES_Y) continue;
    unsigned char* s = tile_state + tile_index_row[cty] + ctx;
    if (*s & 1) continue;
    restore_tile(ctx, cty);
    *s |= 1;
  }
}

/* Store pixels [x0,x1) of framebuffer row `row` from `src`; rows from 236
 * (pixels from 256 on row 236) are staged for write_pixel_tail. */
static void put_row(int row, int x0, int x1, unsigned short* src) {
  if (row < TAIL_ROW || (row == TAIL_ROW && x1 <= TAIL_X)) {
    volatile short* dst = g_pixel_fb + row * SCREEN_W;
    for (int x = x0; x < x1; x++) dst[x] = (short)src[x - x0];
    return;
  }
  int t = row - TAIL_ROW;
  if (row == TAIL_ROW && x0 < TAIL_X) {
    volatile short* dst = g_pixel_fb + row * SCREEN_W;
    for (int x = x0; x < TAIL_X; x++) dst[x] = (short)src[x - x0];
    src += TAIL_X - x0;
    x0 = TAIL_X;
  }
  unsigned short* stage = tail_rows + t * SCREEN_W;
  for (int x = x0; x < x1; x++) stage[x] = src[x - x0];
  if (x0 < tail_lo[t]) tail_lo[t] = x0;
  if (x1 > tail_hi[t]) tail_hi[t] = x1;
}

/* Copy every restored or drawn tile from g_back to the framebuffer, clipped
 * to the camera window (pixels outside it belong to the opposite edge of the
 * ring), then flush the staged kernel-only rows. */
void view_end(void) {
  for (int t = 0; t < 4; t++) {
    tail_lo[t] = SCREEN_W;
    tail_hi[t] = 0;
  }
  int win_x1 = cam_x0 + SCREEN_W;
  int win_y1 = cam_y0 + 240;
  for (int cty = 0; cty < TILES_Y; cty++) {
    unsigned char* st = tile_state + tile_index_row[cty];
    int wy = (base_ty + cty) << 3;
    if (wy >= win_y1) break;
    int ry0 = wy < cam_y0 ? cam_y0 - wy : 0;
    int ry1 = wy + 8 > win_y1 ? win_y1 - wy : 8;
    int bty = base_ty + cty;
    while (bty >= RING_TY) bty -= RING_TY;
    while (bty < 0) bty += RING_TY;
    for (int ctx = 0; ctx < TILES_X; ctx++) {
      if (!st[ctx]) continue;
      int wx = (base_tx + ctx) << 3;
      if (wx >= win_x1) break;
      int rx0 = wx < cam_x0 ? cam_x0 - wx : 0;
      int rx1 = wx + 8 > win_x1 ? win_x1 - wx : 8;
      int btx = base_tx + ctx;
      while (btx >= RING_TX) btx -= RING_TX;
      while (btx < 0) btx += RING_TX;
      unsigned short* src = g_back + canvas_row[cty << 3] + (ctx << 3);
      int frow = bty << 3;
      int fx = btx << 3;
      if (rx0 == 0 && rx1 == 8 && frow + 8 <= TAIL_ROW) {
        unsigned* s = (unsigned*)(src + ry0 * CANVAS_W);
        unsigned* d = (unsigned*)(g_pixel_fb + (frow + ry0) * SCREEN_W + fx);
        for (int r = ry0; r < ry1; r++) {
          d[0] = s[0];
          d[1] = s[1];
          d[2] = s[2];
          d[3] = s[3];
          s += CANVAS_W >> 1;
          d += SCREEN_W >> 1;
        }
        continue;
      }
      for (int r = ry0; r < ry1; r++) put_row(frow + r, fx + rx0, fx + rx1, src + r * CANVAS_W + rx0);
    }
  }
  for (int t = 0; t < 4; t++) {
    if (tail_hi[t] <= tail_lo[t]) continue;
    int row = TAIL_ROW + t;
    write_pixel_tail((unsigned)(row * SCREEN_W + tail_lo[t]), (short*)(tail_rows + t * SCREEN_W + tail_lo[t]),
                     (unsigned)(tail_hi[t] - tail_lo[t]));
  }
  shown_x = cam_x0;
  shown_y = cam_y0;
  view_valid = 1;
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
  draw_image_clipped(id, x, y, flip, VIEW_H);
}

/* draw_image with the bottom edge also clipped at screen row `bottom`
 * (exclusive), for things that rise out of the floor. */
void draw_image_clipped(int id, int x, int y, int flip, int bottom) {
  struct PakImage* im = (struct PakImage*)(g_pak + pak_header(PAKH_IMAGE_OFF)) + id;
  int w = (int)im->w;
  int h = (int)im->h;
  unsigned short* src = (unsigned short*)(g_pak + im->off);
  if (bottom > VIEW_H) bottom = VIEW_H;
  int y0 = y < HUD_PIXEL_ROWS ? HUD_PIXEL_ROWS : y;
  int y1 = y + h > bottom ? bottom : y + h;
  int x0 = x < 0 ? 0 : x;
  int x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
  if (x0 >= x1 || y0 >= y1) return;
  view_touch(x0, y0, x1, y1);
  for (int py = y0; py < y1; py++) {
    unsigned short* row = src + (py - y) * w;
    unsigned short* dst = g_back + g_row[py];
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
  view_touch(cx - 3, y + bob, cx + 4, y + bob + 4);
  for (int r = 0; r < 4; r++) {
    int py = y + r + bob;
    if (py < HUD_PIXEL_ROWS || py >= VIEW_H) continue;
    unsigned short* dst = g_back + g_row[py];
    for (int dx = -r; dx <= r; dx++) {
      int px = cx + dx;
      if (px >= 0 && px < SCREEN_W) dst[px] = 0x0EEF;
    }
  }
}

/* Start composing a full-screen card off screen: the card background goes
 * into the back buffer, and text draws there until canvas_show(). Drawing
 * cards and menus straight to the display showed them building up. */
void canvas_card(void) {
  tl_copy_words((void*)g_back, (void*)(g_pak + pak_header(PAKH_CARD_OFF)), (SCREEN_W * VIEW_H) >> 1);
  g_canvas = (volatile short*)g_back;
}

/* Show the composed card in one copy and send text back to the display. */
void canvas_show(void) {
  if (scroll_x != 0 || scroll_y != 0) {
    set_pixel_scroll(0, 0);
    scroll_x = 0;
    scroll_y = 0;
  }
  tl_copy_words((void*)g_pixel_fb, (void*)g_back, (SCREEN_W * VIEW_H) >> 1);
  g_canvas = g_pixel_fb;
  view_valid = 0;          /* the play view is redrawn in full afterwards */
}

/* ---- film stock ---- */

static unsigned grain_seed = 1937;
static int scratch_x = -1;
static int scratch_ticks;

static unsigned grain_rand(void) {
  grain_seed = grain_seed * 1103515245u + 12345u;
  return grain_seed >> 8;
}

/* Dust and scratches on the play canvas: a few dozen bright flecks each
 * frame and, now and then, a thin vertical scratch that lingers for a few
 * frames, like a print that has been through the projector too often.
 * Brightening keeps each pixel's tint: two levels added per channel,
 * saturating at 15 by masking before the add. */
void film_fx(int clock) {
  for (int i = 0; i < 28; i++) {
    unsigned r = grain_rand();
    int x = (int)(r % SCREEN_W);
    int y = HUD_PIXEL_ROWS + (int)((r >> 9) % (VIEW_H - HUD_PIXEL_ROWS));
    view_touch(x, y, x + 1, y + 1);
    unsigned short* p = g_back + g_row[y] + x;
    unsigned short c = *p;
    *p = (unsigned short)((c & 0x0CCC) + 0x0333);
  }
  if (scratch_ticks > 0) {
    scratch_ticks--;
    view_touch(scratch_x, HUD_PIXEL_ROWS, scratch_x + 1, VIEW_H);
    for (int y = HUD_PIXEL_ROWS; y < VIEW_H; y++) {
      if (((y + clock) & 7) == 0) continue;
      unsigned short* p = g_back + g_row[y] + scratch_x;
      *p = (unsigned short)((*p & 0x0EEE) + 0x0111);
    }
  } else if ((grain_rand() & 255) < 3) {
    scratch_x = 8 + (int)(grain_rand() % (SCREEN_W - 16));
    scratch_ticks = 6 + (int)(grain_rand() & 7);
  }
}
