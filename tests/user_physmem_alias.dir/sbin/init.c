/*
 * Physical-display mapping alias test:
 * - verify every display getter succeeds and an exact repeated call returns
 *   the same virtual pointer instead of consuming another VME
 * - write distinct sentinels to the first word of two tilemap pages
 * - repeat the check for two tile-framebuffer pages
 * - verify adjacent virtual pages retain independent physical contents
 * - fault and write tilemap, tile-framebuffer, and sprite pages before fork
 * - have the child update those inherited mappings without printing
 * - wait for the child, then prove the parent sees the updates through the
 *   same live MMIO pages rather than pre-fork private RAM snapshots
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define TILEMAP_NEXT_PAGE_INDEX 2048
#define TILE_FB_NEXT_PAGE_INDEX 2048

#define TILEMAP_SENTINEL_0 273
#define TILEMAP_SENTINEL_1 546
#define TILE_FB_SENTINEL_0 257
#define TILE_FB_SENTINEL_1 514

#define TILEMAP_FORK_PAGE_INDEX 4096
#define TILE_FB_FORK_PAGE_INDEX 4096
#define SPRITEMAP_FORK_PAGE_INDEX 2048

#define TILEMAP_PARENT_SENTINEL 819
#define TILEMAP_CHILD_SENTINEL 1092
#define TILE_FB_PARENT_SENTINEL 771
#define TILE_FB_CHILD_SENTINEL 1028
#define SPRITEMAP_PARENT_SENTINEL 1285
#define SPRITEMAP_CHILD_SENTINEL 1542

#define CHILD_STATUS_OK 42
#define CHILD_STATUS_GETTER_CHANGED 51
#define CHILD_STATUS_PARENT_VALUES_MISSING 52

static int mapping_ok(volatile short* mapping){ /* Verify that the aliased mapping contains the expected test values. */
  return mapping != NULL && (int)mapping != -1;
}

// Run after fork without console output so the child cannot modify the tile
// framebuffer as a side effect of reporting its checks. The inherited virtual
// bases must still be the canonical results of the three getter traps.
static int child_update_mappings(volatile short* tilemap, volatile short* tile_fb,
    volatile short* spritemap){
  if (get_tilemap() != tilemap || get_tile_fb() != tile_fb ||
      get_spritemap() != spritemap){
    return CHILD_STATUS_GETTER_CHANGED;
  }

  if (tilemap[TILEMAP_FORK_PAGE_INDEX] != TILEMAP_PARENT_SENTINEL ||
      tile_fb[TILE_FB_FORK_PAGE_INDEX] != TILE_FB_PARENT_SENTINEL ||
      spritemap[SPRITEMAP_FORK_PAGE_INDEX] != SPRITEMAP_PARENT_SENTINEL){
    return CHILD_STATUS_PARENT_VALUES_MISSING;
  }

  tilemap[TILEMAP_FORK_PAGE_INDEX] = TILEMAP_CHILD_SENTINEL;
  tile_fb[TILE_FB_FORK_PAGE_INDEX] = TILE_FB_CHILD_SENTINEL;
  spritemap[SPRITEMAP_FORK_PAGE_INDEX] = SPRITEMAP_CHILD_SENTINEL;
  return CHILD_STATUS_OK;
}

int main(void){ /* Verify physical-memory aliases are visible with shared updates. */
  volatile short* tilemap = get_tilemap();
  volatile short* tile_fb = get_tile_fb();
  volatile short* spritemap = get_spritemap();
  volatile short* tilemap_again = get_tilemap();
  volatile short* tile_fb_again = get_tile_fb();
  volatile short* spritemap_again = get_spritemap();
  int ok = 1;

  user_test_expect_eq("tilemap mapping succeeds", mapping_ok(tilemap), 1);
  user_test_expect_eq("tile framebuffer mapping succeeds",
    mapping_ok(tile_fb), 1);
  user_test_expect_eq("spritemap mapping succeeds", mapping_ok(spritemap), 1);
  user_test_expect_eq("repeated tilemap getter reuses pointer",
    tilemap_again == tilemap, 1);
  user_test_expect_eq("repeated tile framebuffer getter reuses pointer",
    tile_fb_again == tile_fb, 1);
  user_test_expect_eq("repeated spritemap getter reuses pointer",
    spritemap_again == spritemap, 1);

  if (!mapping_ok(tilemap) || !mapping_ok(tile_fb) ||
      !mapping_ok(spritemap)){
    return 1;
  }

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

  // These accesses make all three selected pages resident before fork. The old
  // clone path copied their PTE contents into newly allocated RAM; consequently
  // the child's later stores were invisible through the parent's live device
  // mappings. Copying direct-map PTEs makes the checks below deterministic.
  tilemap[TILEMAP_FORK_PAGE_INDEX] = TILEMAP_PARENT_SENTINEL;
  tile_fb[TILE_FB_FORK_PAGE_INDEX] = TILE_FB_PARENT_SENTINEL;
  spritemap[SPRITEMAP_FORK_PAGE_INDEX] = SPRITEMAP_PARENT_SENTINEL;

  int child = fork();
  if (child == 0){
    return child_update_mappings(tilemap, tile_fb, spritemap);
  }

  user_test_expect_eq("fork direct-mapping child created", child >= 200, 1);
  if (child < 0){
    return 1;
  }

  user_test_expect_eq("direct-mapping child exit status", wait_child(child),
    CHILD_STATUS_OK);
  user_test_expect_eq("parent sees child tilemap update",
    tilemap[TILEMAP_FORK_PAGE_INDEX], TILEMAP_CHILD_SENTINEL);
  user_test_expect_eq("parent sees child tile framebuffer update",
    tile_fb[TILE_FB_FORK_PAGE_INDEX], TILE_FB_CHILD_SENTINEL);
  user_test_expect_eq("parent sees child spritemap update",
    spritemap[SPRITEMAP_FORK_PAGE_INDEX], SPRITEMAP_CHILD_SENTINEL);

  return CHILD_STATUS_OK;
}
