/*
 * Exercise the actual save codec and slot selection against real pack geometry.
 * The only substitutions are documented guest file operations. Fail writes at
 * every byte boundary, corrupt/truncate every byte of the newest record, and
 * require the older checkpoint to survive. Corrupt loads must never mutate
 * their caller's progress, even when a forged record has a correct checksum.
 */
#include "sim.c"
#undef SAVE_MAGIC
#define RETAKE_SAVE_NATIVE
#include "../save.c"
#undef open
#undef open_existing
#undef read
#undef write
#undef close
#undef seek
#undef truncate
#undef errno

/* Two independent guest files, with explicit lengths and descriptor offsets. */
struct TestFile {
  unsigned char bytes[512];
  int length;
  int exists;
  int cursor;
};
static struct TestFile files[2];
static int io_chunk = 1024;
static int write_budget = -1;
static int bad_read = -1;
static int bad_open = -1;
static int bad_close = -1;
static int bad_seek;
static int bad_truncate;
static int failures;
int saveio_errno;

/* Paths must match the two production slots, never a host filesystem path. */
static int file_index(char* path) {
  if (!strcmp(path, "/retake/save0.dat")) return 0;
  if (!strcmp(path, "/retake/save1.dat")) return 1;
  fprintf(stderr, "unexpected save path: %s\n", path);
  abort();
}

/* Creating open preserves previous content and begins at offset zero. */
int saveio_open(char* path) {
  int i = file_index(path);
  if (bad_open == i) return -1;
  files[i].exists = 1;
  files[i].cursor = 0;
  return 3 + i;
}

/* Only a genuine missing path reports ENOENT; injected failures are uncertain. */
int saveio_existing(char* path) {
  int i = file_index(path);
  if (bad_open == i) return -1;
  if (!files[i].exists) { saveio_errno = ENOENT; return -1; }
  files[i].cursor = 0;
  return 3 + i;
}

/* EOF and positive short reads match the guest contract, not POSIX assumptions. */
int saveio_read(int fd, void* dst, unsigned size) {
  int i = fd - 3;
  if (i < 0 || i > 1 || bad_read == i) return -1;
  struct TestFile* f = &files[i];
  int n = (int)size;
  if (n > io_chunk) n = io_chunk;
  if (n > f->length - f->cursor) n = f->length - f->cursor;
  memcpy(dst, f->bytes + f->cursor, (size_t)n);
  f->cursor += n;
  return n;
}

/* Fail after an exact committed prefix to model every possible interruption. */
int saveio_write(int fd, void* src, unsigned size) {
  int i = fd - 3;
  if (i < 0 || i > 1 || write_budget == 0) return -1;
  struct TestFile* f = &files[i];
  int n = (int)size;
  if (n > io_chunk) n = io_chunk;
  if (write_budget >= 0 && n > write_budget) n = write_budget;
  if (f->cursor + n > (int)sizeof(f->bytes)) return -1;
  memcpy(f->bytes + f->cursor, src, (size_t)n);
  f->cursor += n;
  if (f->cursor > f->length) f->length = f->cursor;
  if (write_budget >= 0) write_budget -= n;
  return n;
}

/* Closing may fail independently of whether a complete record reached the file. */
int saveio_close(int fd) { return fd - 3 == bad_close ? -1 : 0; }

/* The production writer requests an explicit beginning-of-file offset only. */
int saveio_seek(int fd, int offset, int whence) {
  if (bad_seek || fd < 3 || fd > 4 || offset != 0 || whence != SEEK_SET) return -1;
  files[fd - 3].cursor = 0;
  return 0;
}

/* Truncation only shrinks, as specified by the Dioptase syscall. */
int saveio_truncate(int fd, unsigned size) {
  if (bad_truncate || fd < 3 || fd > 4 || size > (unsigned)files[fd - 3].length) return -1;
  files[fd - 3].length = (int)size;
  return 0;
}

/* Keep diagnostics specific enough to identify the lost recovery contract. */
static void expect(int ok, char* reason) {
  if (!ok) { fprintf(stderr, "***FAIL retake save: %s\n", reason); failures++; }
}

/* Remove injected failures without changing either slot's stored bytes. */
static void clear_faults(void) {
  io_chunk = 1024;
  write_budget = -1;
  bad_read = -1;
  bad_open = -1;
  bad_close = -1;
  bad_seek = 0;
  bad_truncate = 0;
}

/* Start each scenario with an ordinary clear mirror-compatible checkpoint. */
static void fresh(void) {
  clear_faults();
  memset(files, 0, sizeof(files));
  game_new(ROOM_S1_ENTRY);
  g_prog.scrip = 10;
}

/* Round trips select the newest serial and tolerate one-byte read/write chunks. */
static void test_round_trip(void) {
  fresh();
  io_chunk = 1;
  expect(progress_save(&g_prog) == 0, "one-byte writes produce a verified first save");
  struct Progress loaded;
  expect(progress_load(&loaded) == 0 && !memcmp(&loaded, &g_prog, sizeof(loaded)),
         "one-byte reads round-trip every progress field");
  g_prog.scrip = 20;
  expect(progress_save(&g_prog) == 0 && files[0].exists && files[1].exists,
         "the second save leaves the first slot present");
  expect(progress_load(&loaded) == 0 && loaded.scrip == 20, "load chooses the newer checkpoint");
}

/* Every interrupted overwrite must preserve the other slot's latest checkpoint. */
static void test_torn_writes(void) {
  fresh();
  expect(progress_save(&g_prog) == 0, "prepare the original checkpoint");
  struct TestFile first[2];
  memcpy(first, files, sizeof(files));
  for (int pass = 0; pass < 2; pass++) {
    for (int cut = 0; cut < SAVE_BYTES; cut++) {
      memcpy(files, first, sizeof(files));
      g_prog.scrip = pass ? 30 : 20;
      write_budget = cut;
      expect(progress_save(&g_prog) == -1, "a torn write is never reported as saved");
      clear_faults();
      struct Progress loaded;
      int latest = pass ? 1 : 0;
      expect(files[latest].length == first[latest].length &&
             !memcmp(files[latest].bytes, first[latest].bytes, sizeof(files[latest].bytes)),
             "the previous latest slot stays byte-for-byte intact during interrupted writes");
      int valid = progress_load(&loaded) == 0;
      struct Progress previous = g_prog;
      previous.scrip = pass ? 20 : 10;
      expect(valid && (!memcmp(&loaded, &previous, sizeof(loaded)) ||
                       !memcmp(&loaded, &g_prog, sizeof(loaded))),
             "recovery returns exactly the old or complete new checkpoint, never mixed progress");
    }
    memcpy(files, first, sizeof(files));
    g_prog.scrip = 20;
    expect(progress_save(&g_prog) == 0, "prepare two existing slots for overwrite tests");
    memcpy(first, files, sizeof(files));
  }
}

/* CRC and exact record length reject corruption, including the generation field. */
static void test_corruption(void) {
  fresh();
  progress_save(&g_prog);
  g_prog.scrip = 20;
  progress_save(&g_prog);
  struct TestFile good = files[1];
  struct Progress loaded;
  for (int byte = 0; byte < SAVE_BYTES; byte++) {
    files[1] = good;
    files[1].bytes[byte] ^= 1;
    expect(progress_load(&loaded) == 0 && loaded.scrip == 10,
           "a corrupt newest record falls back to the prior checkpoint");
    files[1] = good;
    files[1].length = byte;
    expect(progress_load(&loaded) == 0 && loaded.scrip == 10,
           "a truncated newest record falls back to the prior checkpoint");
  }
  files[0].length = 0;
  files[1].length = 0;
  memset(&loaded, 0xA5, sizeof(loaded));
  struct Progress sentinel = loaded;
  expect(progress_load(&loaded) == -1 && !memcmp(&loaded, &sentinel, sizeof(loaded)),
         "failure leaves the caller's live progress unchanged");
}

/* Build deliberately forged but checksummed data to test semantic validation. */
static void store_record(int slot, struct SaveRecord* record) {
  record->word[5] = record_crc(record);
  for (int i = 0; i < SAVE_WORDS; i++) {
    unsigned word = record->word[i];
    for (int b = 0; b < 4; b++) { files[slot].bytes[i * 4 + b] = word & 255u; word >>= 8; }
  }
  files[slot].exists = 1;
  files[slot].length = SAVE_BYTES;
}

/* Valid checksums cannot legitimize an incompatible world or unsafe progress. */
static void test_invalid_progress(void) {
  fresh();
  progress_save(&g_prog);
  struct SaveRecord good;
  save_read_slot(0, &good);
  unsigned changes[10] = {0, 10000, 0x80000000u, 4, 239, PAK_ROOM_COUNT, 0xFFFFFFFFu, 208, 0, 0};
  int fields[10] = {6, 7, 8, 9, 10, 11, 12, 13, 2, 3};
  for (int i = 0; i < 10; i++) {
    struct SaveRecord bad = good;
    bad.word[fields[i]] = changes[i];
    store_record(0, &bad);
    struct Progress output;
    memset(&output, 0x5A, sizeof(output));
    struct Progress sentinel = output;
    expect(progress_load(&output) == -1 && !memcmp(&output, &sentinel, sizeof(output)),
           "a forged invalid field or blocked checkpoint is rejected without mutation");
  }
  g_prog.save_x = -1;
  expect(progress_save(&g_prog) == -1, "invalid caller progress cannot replace a save");
}

/* Unknown I/O failures must not be mistaken for a safely replaceable empty slot. */
static void test_io_failures(void) {
  fresh();
  progress_save(&g_prog);
  unsigned char original[512];
  memcpy(original, files[0].bytes, sizeof(original));
  g_prog.scrip = 30;
  bad_read = 0;
  expect(progress_save(&g_prog) == -1 && !memcmp(original, files[0].bytes, sizeof(original)),
         "an unreadable previous slot is never overwritten as if missing");
  clear_faults();
  bad_open = 1;
  expect(progress_save(&g_prog) == -1, "an unknown open failure aborts saving");
  clear_faults();
  bad_seek = 1;
  expect(progress_save(&g_prog) == -1, "failed seek cannot report success");
  clear_faults();
  bad_truncate = 1;
  expect(progress_save(&g_prog) == -1, "failed final length update cannot report success");
  clear_faults();
  bad_close = 1;
  expect(progress_save(&g_prog) == -1, "failed close cannot report success");
  clear_faults();
  expect(!memcmp(original, files[0].bytes, sizeof(original)), "all failed saves preserve the original slot");
}

/* Generation wrap must not resurrect the pre-wrap checkpoint. */
static void test_serial_wrap(void) {
  fresh();
  progress_save(&g_prog);
  struct SaveRecord r;
  save_read_slot(0, &r);
  r.word[3] = 0xFFFFFFFEu;
  store_record(0, &r);
  g_prog.scrip = 20;
  expect(progress_save(&g_prog) == 0, "save immediately before serial wrap succeeds");
  g_prog.scrip = 30;
  expect(progress_save(&g_prog) == 0, "save across serial wrap succeeds");
  struct Progress loaded;
  expect(progress_load(&loaded) == 0 && loaded.scrip == 30, "serial wrap keeps the newest save selected");
}

/* Run file-recovery tests against the same generated room data as the game. */
int main(void) {
  if (sim_open("../retake.pak")) return 1;
  test_round_trip();
  test_torn_writes();
  test_corruption();
  test_invalid_progress();
  test_io_failures();
  test_serial_wrap();
  sim_close();
  if (!failures) puts("***PASS retake recoverable saves, every-byte interruption and corruption");
  return failures != 0;
}
