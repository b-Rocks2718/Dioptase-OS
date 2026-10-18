/*
 * Recoverable saves for a single user-mode owner. docs/syscalls.md,
 * Filesystem and Pipes: open creates without truncation, reads/writes can be
 * short, seek has explicit offsets, and truncate only shrinks. No atomic
 * replacement or power-loss durability is specified. We overwrite only the
 * older/invalid slot, retain the newest valid file, and verify the new record
 * by reading it back. This covers interrupted writes; it cannot promise that
 * the filesystem survives arbitrary power loss or concurrent writers.
 *
 * The disk format is explicitly little endian, independent of struct layout:
 * six header words (magic, version, world hash, serial, payload words, CRC32),
 * then nine scalar progress words, 64 flag words and eight visited words.
 * CRC32/IEEE covers every word except its own field, including the serial.
 */
#ifdef RETAKE_SAVE_NATIVE
#include "tests/save_io.h"
#else
#include "../crt/sys.h"
#include "../crt/errno.h"
#endif
#include "retake.h"
#include "save.h"
#include "pak.h"

#define SAVE_MAGIC 0x32565352u       /* RSV2; legacy raw-struct saves are not imported */
#define SAVE_VERSION 2
#define SAVE_WORDS 87
#define SAVE_BYTES 348
#define SAVE_PAYLOAD_WORDS 81
#define SAVE_CRC_WORD 5
#define SAVE_PAYLOAD_START 6
#define SAVE_CRC_POLYNOMIAL 0xEDB88320u
#define SAVE_BASE_HEALTH 5          /* game_new's starting health */
#define SAVE_SHOP_HEALTH 8          /* menus.c stops selling frames at eight */

/* Aligned decoded disk words plus a validity result; no native pointers persist. */
struct SaveRecord {
  unsigned word[87];
  int valid;
};

static char* save_message = "retake save: no failure";

/* Stable absolute paths avoid changing the foreground process's working directory. */
static char* slot_path(int slot) {
  return slot ? "/retake/save1.dat" : "/retake/save0.dat";
}

/* Expose a contextual failure without printing over the game's framebuffer. */
char* progress_error(void) { return save_message; }

/* Bounded byte transfers tolerate positive short results and reject zero progress. */
static int transfer(int fd, unsigned char* bytes, int size, int writing) {
  int done = 0;
  while (done < size) {
    int n = writing ? write(fd, bytes + done, size - done) : read(fd, bytes + done, size - done);
    if (n < 0 || n > size - done) return -1;
    if (!n) return 0;
    done += n;
  }
  return 1;
}

/* Avoid a lookup table and multiply: manual saves are infrequent, bounded work. */
static unsigned record_crc(struct SaveRecord* record) {
  unsigned crc = 0xFFFFFFFFu;
  for (int i = 0; i < SAVE_WORDS; i++) {
    if (i == SAVE_CRC_WORD) continue;
    unsigned value = record->word[i];
    for (int byte = 0; byte < 4; byte++) {
      crc ^= value & 255u;
      value >>= 8;
      for (int bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ ((crc & 1u) ? SAVE_CRC_POLYNOMIAL : 0u);
    }
  }
  return ~crc;
}

/* Explicit fields keep compiler padding and future struct additions off disk. */
static void encode_progress(struct SaveRecord* r, struct Progress* p) {
  r->word[6] = p->max_health;
  r->word[7] = p->scrip;
  r->word[8] = p->abilities;
  r->word[9] = p->take_slots;
  r->word[10] = p->take_frames;
  r->word[11] = p->save_room;
  r->word[12] = p->save_x;
  r->word[13] = p->save_y;
  r->word[14] = p->play_ticks;
  for (int i = 0; i < 64; i++) r->word[15 + i] = p->flags[i];
  for (int i = 0; i < 8; i++) r->word[79 + i] = p->visited[i];
}

/* Decode only into temporary storage until the complete record is accepted. */
static void decode_progress(struct SaveRecord* r, struct Progress* p) {
  p->max_health = r->word[6];
  p->scrip = r->word[7];
  p->abilities = r->word[8];
  p->take_slots = r->word[9];
  p->take_frames = r->word[10];
  p->save_room = r->word[11];
  p->save_x = r->word[12];
  p->save_y = r->word[13];
  p->play_ticks = r->word[14];
  for (int i = 0; i < 64; i++) p->flags[i] = r->word[15 + i];
  for (int i = 0; i < 8; i++) p->visited[i] = r->word[79 + i];
}

/* A loaded body must fit terrain and the gates/crumbles restored on room entry. */
static int checkpoint_valid(struct Progress* p) {
  if (p->save_room < 0 || p->save_room >= pak_room_count()) return 0;
  struct PakRoom* room = pak_room(p->save_room);
  if (p->save_x < 0 || p->save_y < 0 ||
      p->save_x > room->w * TILE - PLAYER_W || p->save_y > room->h * TILE - PLAYER_H) return 0;
  int x1 = (p->save_x + PLAYER_W - 1) / TILE;
  int y1 = (p->save_y + PLAYER_H - 1) / TILE;
  for (int y = p->save_y / TILE; y <= y1; y++)
    for (int x = p->save_x / TILE; x <= x1; x++)
      if (g_pak[room->col_off + y * room->w + x] != COL_AIR) return 0;
  short* raw = (short*)(g_pak + room->entities_off);
  for (int i = 0; i < room->entity_count; i++) {
    short* e = raw + i * 8;
    if (e[0] != ENT_GATE && e[0] != ENT_CRUMBLE) continue;
    int height = e[0] == ENT_GATE && e[4] > 0 ? e[4] : 1;
    if (p->save_x < (e[1] + 1) * TILE && p->save_x + PLAYER_W > e[1] * TILE &&
        p->save_y < (e[2] + height) * TILE && p->save_y + PLAYER_H > e[2] * TILE) return 0;
  }
  return 1;
}

/* Numeric bounds reflect actual upgrade rules; flag words are opaque bitsets. */
static int save_progress_valid(struct Progress* p) {
  int allowed = AB_JUMPCUT | AB_WIRE | AB_GLIDE | AB_REWIND | AB_OVERCRANK | AB_EXTRA_REEL;
  int max_health = SAVE_SHOP_HEALTH;
  for (int i = 0; i < pak_room_count(); i++) {
    struct PakRoom* room = pak_room(i);
    short* raw = (short*)(g_pak + room->entities_off);
    for (int j = 0; j < room->entity_count; j++)
      if (raw[j * 8] == ENT_FRAME) max_health++;
  }
  if (p->max_health < SAVE_BASE_HEALTH || p->max_health > max_health ||
      p->scrip < 0 || p->scrip > 9999 || (p->abilities & ~allowed) ||
      p->take_slots < 1 || p->take_slots > TAKE_MAX_SLOTS ||
      (p->take_frames != TAKE_BASE_FRAMES && p->take_frames != 360 && p->take_frames != TAKE_MAX_FRAMES)) return 0;
  for (int i = pak_room_count(); i < 256; i++)
    if ((p->visited[i >> 5] >> (i & 31)) & 1u) return 0;
  return checkpoint_valid(p);
}

/* CRC protects opaque bitsets too; a valid checksum alone cannot validate geometry. */
static int save_record_valid(struct SaveRecord* r) {
  if (r->word[0] != SAVE_MAGIC || r->word[1] != SAVE_VERSION || r->word[2] != PAK_WORLD_HASH ||
      !r->word[3] || r->word[4] != SAVE_PAYLOAD_WORDS || r->word[5] != record_crc(r)) return 0;
  struct Progress candidate;
  decode_progress(r, &candidate);
  return save_progress_valid(&candidate);
}

/* Return -1 for uncertain I/O, 0 for absent/invalid, 1 for a validated record. */
static int save_read_slot(int slot, struct SaveRecord* r) {
  r->valid = 0;
  errno = 0;
  int fd = open_existing(slot_path(slot));
  if (fd < 0) return errno == ENOENT ? 0 : -1;
  unsigned char bytes[348];
  int result = transfer(fd, bytes, SAVE_BYTES, 0);
  if (result == 1) {
    unsigned char extra[1];
    int tail = read(fd, extra, 1);
    if (tail < 0) result = -1;
    else if (tail != 0) result = 0;
  }
  if (close(fd) < 0) return -1;
  if (result != 1) return result;
  for (int i = 0; i < SAVE_WORDS; i++) {
    int at = i * 4;
    r->word[i] = (unsigned)bytes[at] | ((unsigned)bytes[at + 1] << 8) |
                 ((unsigned)bytes[at + 2] << 16) | ((unsigned)bytes[at + 3] << 24);
  }
  r->valid = save_record_valid(r);
  return r->valid;
}

/* Serial arithmetic handles wrap while keeping the two adjacent generations ordered. */
static int newer(unsigned a, unsigned b) {
  unsigned difference = a - b;
  return difference != 0 && difference < 0x80000000u;
}

/* Select the newest readable valid slot, tolerating corruption in its partner. */
int progress_load(struct Progress* output) {
  struct SaveRecord a;
  struct SaveRecord b;
  save_read_slot(0, &a);
  save_read_slot(1, &b);
  if (!a.valid && !b.valid) {
    save_message = "retake save: neither /retake/save0.dat nor save1.dat has a compatible valid checkpoint";
    return -1;
  }
  struct SaveRecord* best = &a;
  if (b.valid && (!a.valid || newer(b.word[3], a.word[3]))) best = &b;
  decode_progress(best, output);
  save_message = "retake save: no failure";
  return 0;
}

/* Preserve the newest file even if opening, writing, closing or verifying fails. */
int progress_save(struct Progress* p) {
  if (!save_progress_valid(p)) {
    save_message = "retake save: invalid progress or blocked checkpoint; return to a mirror before retrying";
    return -1;
  }
  struct SaveRecord a;
  struct SaveRecord b;
  int ra = save_read_slot(0, &a);
  int rb = save_read_slot(1, &b);
  if (ra < 0 || rb < 0) {
    save_message = ra < 0 ? "retake save: reading /retake/save0.dat failed; previous save left untouched" :
                           "retake save: reading /retake/save1.dat failed; previous save left untouched";
    return -1;
  }
  int newest = !a.valid ? 1 : (b.valid && newer(b.word[3], a.word[3]));
  unsigned serial = 1;
  int target = 0;
  if (a.valid || b.valid) {
    serial = (newest ? b.word[3] : a.word[3]) + 1;
    if (!serial) serial = 1;
    target = 1 - newest;
  }
  struct SaveRecord next;
  next.word[0] = SAVE_MAGIC;
  next.word[1] = SAVE_VERSION;
  next.word[2] = PAK_WORLD_HASH;
  next.word[3] = serial;
  next.word[4] = SAVE_PAYLOAD_WORDS;
  encode_progress(&next, p);
  next.word[5] = record_crc(&next);
  unsigned char bytes[348];
  for (int i = 0; i < SAVE_WORDS; i++) {
    unsigned value = next.word[i];
    for (int j = 0; j < 4; j++) { bytes[i * 4 + j] = value & 255u; value >>= 8; }
  }
  int fd = open(slot_path(target));
  if (fd < 0) {
    save_message = target ? "retake save: cannot open /retake/save1.dat for writing" :
                            "retake save: cannot open /retake/save0.dat for writing";
    return -1;
  }
  int ok = seek(fd, 0, SEEK_SET) == 0;
  if (ok) ok = transfer(fd, bytes, SAVE_BYTES, 1) == 1;
  if (ok) ok = truncate(fd, SAVE_BYTES) == 0;
  if (close(fd) < 0) ok = 0;
  if (!ok) {
    save_message = target ? "retake save: writing /retake/save1.dat failed; previous slot retained" :
                            "retake save: writing /retake/save0.dat failed; previous slot retained";
    return -1;
  }
  struct SaveRecord verify;
  int verified = save_read_slot(target, &verify) == 1;
  for (int i = 0; i < SAVE_WORDS && verified; i++)
    if (verify.word[i] != next.word[i]) verified = 0;
  if (!verified) {
    save_message = target ? "retake save: verification of /retake/save1.dat failed; previous slot retained" :
                            "retake save: verification of /retake/save0.dat failed; previous slot retained";
    return -1;
  }
  save_message = "retake save: no failure";
  return 0;
}
