#include "../../../crt/sys.h"

#define TILEMAP_NEXT_PAGE_INDEX 2048
#define TILE_FB_NEXT_PAGE_INDEX 2048

#define TILEMAP_SENTINEL_0 273
#define TILEMAP_SENTINEL_1 546
#define TILE_FB_SENTINEL_0 257
#define TILE_FB_SENTINEL_1 514

int main(void){
  short* tilemap = get_tilemap();
  short* tile_fb = get_tile_fb();
  int ok = 1;

  tilemap[0] = TILEMAP_SENTINEL_0;
  tilemap[TILEMAP_NEXT_PAGE_INDEX] = TILEMAP_SENTINEL_1;

  test_syscall(tilemap[0]);
  test_syscall(tilemap[TILEMAP_NEXT_PAGE_INDEX]);

  if (tilemap[0] != TILEMAP_SENTINEL_0) ok = 0;
  if (tilemap[TILEMAP_NEXT_PAGE_INDEX] != TILEMAP_SENTINEL_1) ok = 0;

  tile_fb[0] = TILE_FB_SENTINEL_0;
  tile_fb[TILE_FB_NEXT_PAGE_INDEX] = TILE_FB_SENTINEL_1;

  test_syscall(tile_fb[0]);
  test_syscall(tile_fb[TILE_FB_NEXT_PAGE_INDEX]);

  if (tile_fb[0] != TILE_FB_SENTINEL_0) ok = 0;
  if (tile_fb[TILE_FB_NEXT_PAGE_INDEX] != TILE_FB_SENTINEL_1) ok = 0;

  test_syscall(ok);

  return 42;
}
