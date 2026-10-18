#include "../crt/sys.h"
#include "../crt/print.h"
#include "retake.h"
#include "pak.h"

/*
 * The asset pack (/retake/retake.pak, built by tools/build.py) is mapped
 * read-only and private, so pages fault in from the file on first touch and
 * nothing is copied up front. Tables are addressed by byte offsets from the
 * start of the file.
 */

#define PAK_PATH "/retake/retake.pak"

unsigned char* g_pak;

/* Header words, in the order build.py writes them. */
#define HDR_MAGIC 0
#define HDR_VERSION 1
#define HDR_REGIONS 2
#define HDR_ROOMS 3
#define HDR_REGION_OFF 4
#define HDR_ROOM_OFF 5
#define HDR_SPRITE_OFF 6
#define HDR_SPRITES 7

static unsigned* header(void) {
  return (unsigned*)g_pak;
}

/* Map the pack. Returns 0, or -1 after printing why (missing file, failed
 * mapping, or a pack built for a different version of the game). */
int pak_open(void) {
  int fd = open_existing(PAK_PATH);
  if (fd < 0) {
    puts("retake: cannot open /retake/retake.pak (run tools/build.py in root/retake)\n");
    return -1;
  }
  void* base = mmap(PAK_SIZE, fd, 0, MMAP_READ);
  close(fd);
  if ((int)base == -1) {
    puts("retake: mmap of /retake/retake.pak failed\n");
    return -1;
  }
  g_pak = (unsigned char*)base;
  unsigned* h = header();
  if (h[HDR_MAGIC] != PAK_MAGIC || h[HDR_VERSION] != PAK_VERSION || h[HDR_ROOMS] != PAK_ROOM_COUNT) {
    puts("retake: /retake/retake.pak does not match this build (rebuild with tools/build.py)\n");
    return -1;
  }
  return 0;
}

int pak_room_count(void) {
  return (int)header()[HDR_ROOMS];
}

struct PakRoom* pak_room(int index) {
  return (struct PakRoom*)(g_pak + header()[HDR_ROOM_OFF]) + index;
}

/* Region entries are stored in ascending region number, starting at 1. */
struct PakRegion* pak_region(int index) {
  return (struct PakRegion*)(g_pak + header()[HDR_REGION_OFF]) + index;
}

struct PakSprite* pak_sprite(int index) {
  return (struct PakSprite*)(g_pak + header()[HDR_SPRITE_OFF]) + index;
}
