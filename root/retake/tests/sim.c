/*
 * Native I/O adapter for the shipping simulation. game.c is included so
 * regression fixtures and solver snapshots can access private state without
 * adding debug entry points to the guest. Movement, combat, takes, gates,
 * progression and tick ordering are never replaced by host approximations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "sim.h"
#include "../pak.h"

unsigned char* g_pak;
struct Room g_room;
int g_room_index = -1;
int g_room_pw;
int g_room_ph;
static unsigned char sim_col[4800];
static int sim_rows[60];
static unsigned char sim_save[4096];
static int sim_save_size;
static int sim_save_cursor;
static int sim_io_limit = 4096;

/* The guest save descriptor is a bounded memory file, never a host fd. */
static int sim_create(char* path) {
  (void)path;
  sim_save_cursor = 0;
  return 3;
}

/* Read-only opens fail until a fixture or game_save has written a record. */
static int sim_existing(char* path) {
  (void)path;
  sim_save_cursor = 0;
  return sim_save_size ? 3 : -1;
}

/* Model documented short reads, including EOF, without host syscall rules. */
static int sim_read(int fd, void* dst, unsigned size) {
  if (fd != 3) return -1;
  int n = (int)size;
  if (n > sim_io_limit) n = sim_io_limit;
  if (n > sim_save_size - sim_save_cursor) n = sim_save_size - sim_save_cursor;
  memcpy(dst, sim_save + sim_save_cursor, (size_t)n);
  sim_save_cursor += n;
  return n;
}

/* Short writes retain their prefix, as the guest file interface permits. */
static int sim_write(int fd, void* src, unsigned size) {
  if (fd != 3) return -1;
  int n = (int)size;
  if (n > sim_io_limit) n = sim_io_limit;
  if (n > (int)sizeof(sim_save) - sim_save_cursor) return -1;
  memcpy(sim_save + sim_save_cursor, src, (size_t)n);
  sim_save_cursor += n;
  if (sim_save_cursor > sim_save_size) sim_save_size = sim_save_cursor;
  return n;
}

/* The memory fixture has no descriptor resources to release. */
static int sim_close_fd(int fd) { return fd == 3 ? 0 : -1; }

/* Suppress guest CRT declarations only inside this native translation unit. */
#define SYS_H
#define PRINT_H
#define open sim_create
#define open_existing sim_existing
#define read sim_read
#define write sim_write
#define close sim_close_fd
#include "../game.c"
#undef open
#undef open_existing
#undef read
#undef write
#undef close

/* Pack words are little endian, aligned 32-bit integers on the supported host. */
unsigned pak_header(int index) { return ((unsigned*)g_pak)[index]; }

/* Use the generated table's count rather than a separate test room list. */
int pak_room_count(void) { return (int)pak_header(PAKH_ROOM_COUNT); }

/* Room records retain the exact shipping pack layout. */
struct PakRoom* pak_room(int index) {
  return (struct PakRoom*)(g_pak + pak_header(PAKH_ROOM_OFF)) + index;
}

/* Native replays skip art uploads; region selection has no rule side effects. */
void render_region(int region) { (void)region; }

/* Copy mutable collision exactly as room.c does, without composing pixels. */
void room_load(int index) {
  struct PakRoom* r = pak_room(index);
  g_room_index = index;
  g_room_pw = r->w * TILE;
  g_room_ph = r->h * TILE;
  g_room.w = r->w;
  g_room.h = r->h;
  g_room.col = sim_col;
  g_room.row = sim_rows;
  g_room.platform_count = 0;
  g_room.platforms = 0;
  memcpy(sim_col, g_pak + r->col_off, (size_t)(r->w * r->h));
  for (int y = 0; y < r->h; y++) sim_rows[y] = y * r->w;
}

/* Bounds checks use subtraction so malformed offsets cannot wrap. */
static int sim_span(unsigned off, unsigned bytes, unsigned total) {
  return off <= total && bytes <= total - off;
}

/* Fail explicitly instead of testing a stale or incompatible generated pack. */
int sim_open(char* path) {
  unsigned endian = 1;
  if (sizeof(int) != 4 || sizeof(short) != 2 || CHAR_BIT != 8 || *(unsigned char*)&endian != 1) {
    fprintf(stderr, "retake replay: native adapter requires little-endian 32-bit int and 16-bit short\n");
    return -1;
  }
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  g_pak = malloc(PAK_SIZE);
  int valid = g_pak && fread(g_pak, 1, PAK_SIZE, f) == PAK_SIZE && fgetc(f) == EOF;
  fclose(f);
  if (!valid) goto bad;
  if (pak_header(PAKH_MAGIC) != PAK_MAGIC || pak_header(PAKH_VERSION) != PAK_VERSION ||
      pak_header(PAKH_SIZE) != PAK_SIZE || pak_header(PAKH_ROOM_COUNT) != PAK_ROOM_COUNT ||
      (pak_header(PAKH_ROOM_OFF) & 3) ||
      !sim_span(pak_header(PAKH_ROOM_OFF), PAK_ROOM_COUNT * sizeof(struct PakRoom), PAK_SIZE)) goto bad;
  for (int i = 0; i < pak_room_count(); i++) {
    struct PakRoom* r = pak_room(i);
    if (r->w < 1 || r->w > ROOM_MAX_W || r->h < 2 || r->h > ROOM_MAX_H ||
        r->entity_count < 0 || r->entity_count > MAX_ENTS || (r->entities_off & 1) ||
        !sim_span((unsigned)r->col_off, (unsigned)(r->w * r->h), PAK_SIZE) ||
        !sim_span((unsigned)r->entities_off, (unsigned)r->entity_count * 16, PAK_SIZE)) goto bad;
  }
  return 0;
bad:
  fprintf(stderr, "retake replay: %s is malformed or differs from pak.h; rebuild tools/build.py\n", path);
  sim_close();
  return -1;
}

/* No snapshots or room pointers may be used after closing the pack. */
void sim_close(void) { free(g_pak); g_pak = 0; }
