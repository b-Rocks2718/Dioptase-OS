#include "../crt/sys.h"
#include "../crt/print.h"
#include "retake.h"

/*
 * The current room. Loading a room composes its image from the region's 8x8
 * blocks (tools/build.py baked all art into them) and builds a copy shifted
 * left by one pixel, so the per-frame compositor always reads word-aligned
 * rows whatever the camera's x parity.
 */

struct Room g_room;
int g_room_index = -1;
int g_room_pw;
int g_room_ph;
unsigned short* g_room_img;
unsigned short* g_room_img_odd;
/* Per room-image row, bit i set when 64-pixel span i is entirely opaque
 * (g_span_opaque) or entirely transparent (g_span_clear); the _odd tables
 * describe the shifted twin. The compositor copies such spans directly. */
unsigned g_span_opaque[480];
unsigned g_span_clear[480];
unsigned g_span_opaque_odd[480];
unsigned g_span_clear_odd[480];

static unsigned char room_col[ROOM_MAX_TILES];
static int room_rows[ROOM_MAX_H];

/* Anonymous read/write memory, or a message and exit if the kernel has none. */
static void* alloc_pages(unsigned bytes, char* what) {
  void* p = mmap(bytes, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  if ((int)p == -1) {
    int args[1] = {(int)what};
    printf("retake: out of memory allocating the %s buffer\n", args);
    exit(1);
  }
  return p;
}

void room_system_init(void) {
  g_room_img = (unsigned short*)alloc_pages(ROOM_MAX_PW * ROOM_MAX_PH * 2, "room image");
  g_room_img_odd = (unsigned short*)alloc_pages(ROOM_MAX_PW * ROOM_MAX_PH * 2, "shifted room image");
  g_room.col = room_col;
  g_room.row = room_rows;
}

/* Fill the span tables for one image: a span is opaque when no word has a
 * transparency nibble set, clear when every pixel is 0xF000. */
static void classify_spans(unsigned short* img, unsigned* opaque, unsigned* clear) {
  int words_per_row = g_room_pw >> 1;
  unsigned* row = (unsigned*)img;
  for (int y = 0; y < g_room_ph; y++) {
    unsigned op = 0;
    unsigned cl = 0;
    int span = 0;
    for (int x = 0; x < words_per_row; x += 32) {
      unsigned any = 0;
      unsigned all = 0xF000F000;
      int end = x + 32 < words_per_row ? x + 32 : words_per_row;
      for (int i = x; i < end; i++) {
        unsigned w = row[i];
        any |= w & 0xF000F000;
        all &= w;
      }
      if (any == 0) op |= 1u << span;
      if (all == 0xF000F000 && (row[x] == 0xF000F000)) cl |= 1u << span;
      span++;
    }
    opaque[y] = op;
    clear[y] = cl;
    row += words_per_row;
  }
}

void room_load(int index) {
  struct PakRoom* pr = pak_room(index);
  struct PakRegion* region = pak_region_by_id(pr->region);
  unsigned short* blocks = (unsigned short*)(g_pak + region->blocks_off);
  unsigned short* grid = (unsigned short*)(g_pak + pr->blocks_off);
  unsigned char* col = g_pak + pr->col_off;
  int w = pr->w;
  int h = pr->h;
  int pw = w << 3;
  g_room_index = index;
  g_room_pw = pw;
  g_room_ph = h << 3;
  g_room.w = w;
  g_room.h = h;
  int r = 0;
  for (int ty = 0; ty < h; ty++) {
    room_rows[ty] = r;
    r += w;
  }
  for (int i = 0; i < w * h; i++) room_col[i] = col[i];

  /* blocks -> image: each block is 8 rows of 4 words */
  int g = 0;
  int row_base = 0;
  int stride_words = pw >> 1;
  for (int ty = 0; ty < h; ty++) {
    for (int tx = 0; tx < w; tx++) {
      unsigned* src = (unsigned*)(blocks + (grid[g] << 6));
      unsigned* dst = (unsigned*)(g_room_img + row_base + (tx << 3));
      for (int y = 0; y < 8; y++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        src += 4;
        dst += stride_words;
      }
      g++;
    }
    row_base += pw << 3;
  }

  /* the one-pixel-shifted twin; its last column repeats the edge */
  int total = pw * g_room_ph;
  for (int i = 0; i < total - 1; i++) g_room_img_odd[i] = g_room_img[i + 1];
  g_room_img_odd[total - 1] = g_room_img[total - 1];
  classify_spans(g_room_img, g_span_opaque, g_span_clear);
  classify_spans(g_room_img_odd, g_span_opaque_odd, g_span_clear_odd);
}
