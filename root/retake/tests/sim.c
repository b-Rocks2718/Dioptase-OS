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

/* Rule state for search backtracking, never serialized as a game save.
 * File contents are intentionally excluded: search does not invoke game_load. */
struct SimSnapshot {
  struct Player hero;
  struct Takes takes;
  struct Progress progress;
  struct Ent ents[40];
  struct Platform platforms[12];
  unsigned char col[4800];
  int switches[16];
  int room;
  int health;
  unsigned events;
  int pending_script;
  int pending_shop;
  int mover_x[8];
  int mover_y[8];
  int ent_count;
  int boss_index;
  int cycle;
  int entrance_x;
  int entrance_y;
  int platform_count;
};

/* Python allocates this opaque size instead of mirroring the large C structs. */
int sim_snapshot_size(void) { return (int)sizeof(struct SimSnapshot); }

/* Capturing includes mutable gate tiles, encounter phase and take cursors. */
void sim_snapshot(void* dst) {
  struct SimSnapshot* s = dst;
  memset(s, 0, sizeof(*s));
  s->hero = g_hero;
  s->takes = g_takes;
  s->progress = g_prog;
  memcpy(s->ents, g_ents, sizeof(g_ents));
  memcpy(s->platforms, g_platforms, sizeof(g_platforms));
  memcpy(s->col, sim_col, sizeof(sim_col));
  memcpy(s->switches, g_switch, sizeof(g_switch));
  s->room = g_room_index;
  s->health = g_health;
  s->events = g_events;
  s->pending_script = g_pending_script;
  s->pending_shop = g_pending_shop;
  memcpy(s->mover_x, mover_dx, sizeof(mover_dx));
  memcpy(s->mover_y, mover_dy, sizeof(mover_dy));
  s->ent_count = g_ent_count;
  s->boss_index = g_boss ? (int)(g_boss - g_ents) : -1;
  s->cycle = boss_cycle;
  s->entrance_x = entry_x;
  s->entrance_y = entry_y;
  s->platform_count = g_room.platform_count;
}

/* Rebuild internal pointers explicitly; copying a Room pointer is insufficient. */
void sim_restore(void* src) {
  struct SimSnapshot* s = src;
  room_load(s->room);
  g_hero = s->hero;
  g_takes = s->takes;
  g_prog = s->progress;
  memcpy(g_ents, s->ents, sizeof(g_ents));
  memcpy(g_platforms, s->platforms, sizeof(g_platforms));
  memcpy(sim_col, s->col, sizeof(sim_col));
  memcpy(g_switch, s->switches, sizeof(g_switch));
  g_health = s->health;
  g_events = s->events;
  g_pending_script = s->pending_script;
  g_pending_shop = s->pending_shop;
  memcpy(mover_dx, s->mover_x, sizeof(mover_dx));
  memcpy(mover_dy, s->mover_y, sizeof(mover_dy));
  g_ent_count = s->ent_count;
  g_boss = s->boss_index >= 0 ? &g_ents[s->boss_index] : 0;
  boss_cycle = s->cycle;
  entry_x = s->entrance_x;
  entry_y = s->entrance_y;
  room_ox = pak_room(s->room)->wx * TILE;
  room_oy = pak_room(s->room)->wy * TILE;
  g_room.platform_count = s->platform_count;
  g_room.platforms = g_platforms;
}

/* Telemetry is Q8 except room/health/flags/timers; no mutable pointers escape. */
void sim_state(int* out) {
  out[0] = g_room_index;
  out[1] = g_hero.x;
  out[2] = g_hero.y;
  out[3] = g_hero.vx;
  out[4] = g_hero.vy;
  out[5] = g_hero.on_ground;
  out[6] = g_hero.wall_dir;
  out[7] = g_health;
  out[8] = g_prog.abilities;
  out[9] = (int)g_events;
  out[10] = g_takes.recording;
  out[11] = g_takes.recorded;
  out[12] = g_takes.slots[0].active;
  out[13] = g_hero.jump_hold;
  out[14] = g_hero.wall_lock;
  out[15] = g_hero.coyote;
  out[16] = g_hero.air_jump_left;
  out[17] = g_boss ? g_boss->hp : 0;
  out[18] = g_prog.scrip;
  out[19] = g_hero.riding;
}

/* Fixtures retain their abilities across transitions; they are not run proofs. */
void sim_fixture(int room, int x, int y, int abilities) {
  game_new(room);
  g_prog.abilities = abilities;
  game_enter_room(room, x, y);
}
