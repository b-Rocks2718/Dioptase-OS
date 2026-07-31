/*
 * Physical-display mapping alias test:
 * - write distinct sentinels to the first word of two tilemap pages
 * - repeat the check for two tile-framebuffer pages
 * - verify adjacent virtual pages retain independent physical contents
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

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

  user_test_expect_eq("tilemap first-page sentinel", tilemap[0],
    TILEMAP_SENTINEL_0);
  user_test_expect_eq("tilemap second-page sentinel",
    tilemap[TILEMAP_NEXT_PAGE_INDEX], TILEMAP_SENTINEL_1);

  if (tilemap[0] != TILEMAP_SENTINEL_0) ok = 0;
  if (tilemap[TILEMAP_NEXT_PAGE_INDEX] != TILEMAP_SENTINEL_1) ok = 0;

  tile_fb[0] = TILE_FB_SENTINEL_0;
  tile_fb[TILE_FB_NEXT_PAGE_INDEX] = TILE_FB_SENTINEL_1;

  user_test_expect_eq("tile framebuffer first-page sentinel", tile_fb[0],
    TILE_FB_SENTINEL_0);
  user_test_expect_eq("tile framebuffer second-page sentinel",
    tile_fb[TILE_FB_NEXT_PAGE_INDEX], TILE_FB_SENTINEL_1);

  if (tile_fb[0] != TILE_FB_SENTINEL_0) ok = 0;
  if (tile_fb[TILE_FB_NEXT_PAGE_INDEX] != TILE_FB_SENTINEL_1) ok = 0;

  user_test_expect_eq("independent physical pages retain sentinels", ok, 1);

  return 42;
}
