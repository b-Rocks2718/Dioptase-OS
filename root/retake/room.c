#include "../crt/sys.h"
#include "../crt/print.h"
#include "retake.h"

/*
 * The current room. Loading a room composes its image from the region's 8x8
 * blocks (tools/build.py baked all art into them). The renderer then paints
 * the region backdrop into its transparent pixels once (render.c), and the
 * display scrolls the result in hardware.
 */

struct Room g_room;
int g_room_index = -1;
int g_room_pw;
int g_room_ph;
unsigned short* g_room_img;
int g_room_col_base[ROOM_MAX_W];
int g_room_serial;

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
  g_room.col = room_col;
  g_room.row = room_rows;
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

  /* blocks -> image: a block is already 64 pixels row by row, the image's
   * tile layout, so each tile is one 32-word copy (columns of tiles, see
   * retake.h). The grid is stored row by row. */
  int base = 0;
  for (int tx = 0; tx < w; tx++) {
    g_room_col_base[tx] = base;
    base += h << 6;
  }
  int g = 0;
  for (int ty = 0; ty < h; ty++) {
    for (int tx = 0; tx < w; tx++) {
      unsigned* src = (unsigned*)(blocks + (grid[g] << 6));
      unsigned* dst = (unsigned*)(g_room_img + g_room_col_base[tx] + (ty << 6));
      for (int k = 0; k < 32; k++) dst[k] = src[k];
      g++;
    }
  }

  g_room_serial++;   /* the renderer composes the backdrop in and redraws */
}
