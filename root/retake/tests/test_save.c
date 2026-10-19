/*
 * Exercise the actual save codec and slot selection against real pack geometry.
 * The only substitutions are documented guest file operations. Fail writes at
 * every byte boundary, corrupt/truncate every byte of the newest record, and
 * require the older checkpoint to survive. Corrupt loads must never mutate
 * their caller's progress, even when a forged record has a correct checksum.
 */
#include "sim.c"

static int failures;

/* Keep diagnostics specific enough to identify the lost recovery contract. */
static void expect(int ok, char* reason) {
  if (!ok) { fprintf(stderr, "***FAIL retake save: %s\n", reason); failures++; }
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
  game_new(ROOM_S5_LIBRARY);
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (e->kind != ENT_CRUMBLE) continue;
    g_prog.save_x = e->tx * TILE;
    g_prog.save_y = e->ty * TILE;
    expect(progress_save(&g_prog) == -1,
           "checkpoint validation rejects a crumble that becomes solid on re-entry");
    break;
  }
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

/* Exercise the mirror event and title wrappers, not just the standalone codec. */
static void test_game_integration(void) {
  fresh();
  player_init(&g_hero, 96, 196, 0);
  game_tick(IN_INTERACT, IN_INTERACT);
  expect((g_events & GE_SAVE) && !(g_events & GE_SAVE_FAILED),
         "a verified mirror write raises only the saved event");
  unsigned char original[512];
  memcpy(original, files[0].bytes, sizeof(original));
  write_budget = 17;
  game_tick(IN_INTERACT, IN_INTERACT);
  expect((g_events & GE_SAVE_FAILED) && !(g_events & GE_SAVE),
         "a short mirror write requests visible failure instead of a saved card");
  expect(!memcmp(original, files[0].bytes, sizeof(original)),
         "failed mirror interaction retains the previous checkpoint bytes");
  clear_faults();

  struct SimSnapshot before;
  struct SimSnapshot after;
  sim_snapshot(&before);
  expect(game_has_save(), "the title can find the surviving checkpoint");
  sim_snapshot(&after);
  expect(!memcmp(&before, &after, sizeof(before)),
         "checking Continue never changes the live game or room");

  /* A new picture in the same process must read existing serials before saving. */
  game_new(ROOM_S1_ENTRY);
  g_prog.scrip = 40;
  expect(game_save() == 0 && !memcmp(original, files[0].bytes, sizeof(original)),
         "a new picture writes the other slot rather than replacing the only checkpoint");
  g_hero.vx = 512;
  g_hero.vy = -700;
  g_hero.facing = -1;
  g_health = 1;
  g_pending_script = SCRIPT_INTRO;
  g_pending_shop = 1;
  g_overcrank = 70;
  overcrank_cool = 300;
  rewind_cool = 100;
  expect(game_load() == 0 && g_prog.scrip == 40 && g_health == g_prog.max_health,
         "Continue restores the newest complete progress and full checkpoint health");
  expect(g_hero.vx == 0 && g_hero.vy == 0 && g_hero.facing == 1 &&
         g_pending_script == -1 && g_pending_shop == 0 && g_overcrank == 0 &&
         overcrank_cool == 0 && rewind_cool == 0 && rewind_count == 0,
         "Continue starts without old motion, cards, shop requests or ability timers");

  files[0].bytes[0] ^= 1;
  files[1].bytes[0] ^= 1;
  sim_snapshot(&before);
  expect(!game_has_save() && game_load() == -1,
         "two corrupt checkpoints cannot be offered or loaded by the title");
  sim_snapshot(&after);
  expect(!memcmp(&before, &after, sizeof(before)),
         "failed Continue preserves every live rule field, not just progress");
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
  test_game_integration();
  sim_close();
  if (!failures) puts("***PASS retake recoverable saves, every-byte interruption and corruption");
  return failures != 0;
}
