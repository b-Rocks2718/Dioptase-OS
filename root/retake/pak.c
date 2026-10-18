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

/* Header word `index` (PAKH_* in the generated pak.h). */
unsigned pak_header(int index) {
  return ((unsigned*)g_pak)[index];
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
  if (pak_header(PAKH_MAGIC) != PAK_MAGIC || pak_header(PAKH_VERSION) != PAK_VERSION ||
      pak_header(PAKH_ROOM_COUNT) != PAK_ROOM_COUNT || pak_header(PAKH_SIZE) != PAK_SIZE) {
    puts("retake: /retake/retake.pak does not match this build (rebuild with tools/build.py)\n");
    return -1;
  }
  return 0;
}

int pak_room_count(void) {
  return (int)pak_header(PAKH_ROOM_COUNT);
}

struct PakRoom* pak_room(int index) {
  return (struct PakRoom*)(g_pak + pak_header(PAKH_ROOM_OFF)) + index;
}

/* Region entries are stored in ascending region number. */
struct PakRegion* pak_region(int index) {
  return (struct PakRegion*)(g_pak + pak_header(PAKH_REGION_OFF)) + index;
}

/* The region entry for region number `id`, or the first region if absent. */
struct PakRegion* pak_region_by_id(int id) {
  int n = (int)pak_header(PAKH_REGION_COUNT);
  for (int i = 0; i < n; i++) {
    if ((int)pak_region(i)->id == id) return pak_region(i);
  }
  return pak_region(0);
}

struct PakSprite* pak_sprite(int index) {
  return (struct PakSprite*)(g_pak + pak_header(PAKH_SPRITE_OFF)) + index;
}
