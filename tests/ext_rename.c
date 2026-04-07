/*
 * ext2 rename test.
 *
 * Validates:
 * - concurrent same-directory renames of disjoint files preserve inode identity
 *   and file contents
 * - parent-directory mutation stays consistent while several workers rename in
 *   the same directory at once
 *
 * How:
 * - load each worker fixture up front so the test records its expected inode
 *   number and payload
 * - start the workers behind one barrier and have them rename between two
 *   alternate names for several rounds
 * - verify that each worker ends with exactly one visible pathname pointing at
 *   the original inode and contents
 */
#include "../kernel/ext.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

#define RENAME_WORKERS 2
#define RENAME_ROUNDS 4

struct Ext2 fs;

struct RenameWorkerArgs {
  struct Barrier* start;
  struct Barrier* done;
  unsigned id;
  unsigned expected_inumber;
  char* initial_name;
  char* alternate_name;
  char* expected_text;
};

static struct Barrier rename_start_barrier;
static struct Barrier rename_done_barrier;
static unsigned expected_inumbers[RENAME_WORKERS];

static char* initial_names[RENAME_WORKERS] = {
  "slot0_a.txt",
  "slot1_a.txt",
};

static char* alternate_names[RENAME_WORKERS] = {
  "slot0_b.txt",
  "slot1_b.txt",
};

static char* expected_texts[RENAME_WORKERS] = {
  "slot0\n",
  "slot1\n",
};

// Reads the full file contents and appends a trailing NUL so worker checks can
// compare the file payload without depending on directory-entry state.
static char* read_node_text(struct Node* node) {
  unsigned size = node_size_in_bytes(node);
  char* text = malloc(size + 1);
  assert(text != NULL,
    "ext_rename: failed to allocate a file read buffer.\n");

  unsigned cnt = node_read_all(node, 0, size, text);
  assert(cnt == size,
    "ext_rename: failed to read the full file contents.\n");
  text[size] = 0;

  return text;
}

static void rename_worker_fail(unsigned id, unsigned round, char* message) {
  int args[2] = { id, round };
  say("***ext_rename FAIL worker=%d round=%d\n", args);
  panic(message);
}

// Verifies that one worker-owned pathname resolves to the original inode and
// contents while the alternate pathname is absent.
static void assert_name_pair(unsigned id, unsigned expected_inumber,
  char* expected_text, unsigned round, char* present_name, char* absent_name) {
  struct Node* present = node_find(&fs.root, present_name);
  if (present == NULL) {
    rename_worker_fail(id, round,
      "ext_rename: expected renamed pathname is missing.\n");
  }

  if (present->cached->inumber != expected_inumber) {
    node_free(present);
    rename_worker_fail(id, round,
      "ext_rename: rename changed the inode number.\n");
  }

  char* text = read_node_text(present);
  if (!streq(text, expected_text)) {
    free(text);
    node_free(present);
    rename_worker_fail(id, round,
      "ext_rename: rename changed the file contents.\n");
  }
  free(text);
  node_free(present);

  struct Node* absent = node_find(&fs.root, absent_name);
  if (absent != NULL) {
    node_free(absent);
    rename_worker_fail(id, round,
      "ext_rename: both pathnames resolved after one rename.\n");
  }
}

// Repeatedly rename one worker-owned file between two alternate names.
static void rename_worker(void* arg) {
  struct RenameWorkerArgs* args = (struct RenameWorkerArgs*)arg;
  struct Barrier* done = args->done;

  barrier_sync(args->start);

  for (unsigned round = 0; round < RENAME_ROUNDS; ++round) {
    char* from = (round % 2 == 0) ? args->initial_name : args->alternate_name;
    char* to = (round % 2 == 0) ? args->alternate_name : args->initial_name;

    node_rename(&fs.root, from, to);

    if ((round & 1) == 0) {
      yield();
    }
  }

  // threads.c frees Fun->arg in the reaper after this worker returns.
  barrier_sync(done);
}

// Loads one worker fixture before the threaded phase so the test knows the
// inode number and baseline payload that every later rename must preserve.
static unsigned load_worker_fixture(unsigned id) {
  int id_args[1] = { id };
  struct Node* node = node_find(&fs.root, initial_names[id]);
  say("| ext_rename: fixture %d initial lookup done\n", id_args);
  assert(node != NULL,
    "ext_rename: initial worker fixture is missing.\n");

  char* text = read_node_text(node);
  assert(streq(text, expected_texts[id]),
    "ext_rename: worker fixture contents do not match expectations.\n");
  free(text);

  unsigned inumber = node->cached->inumber;
  node_free(node);
  say("| ext_rename: fixture %d initial release done\n", id_args);

  struct Node* alternate = node_find(&fs.root, alternate_names[id]);
  say("| ext_rename: fixture %d alternate lookup done\n", id_args);
  assert(alternate == NULL,
    "ext_rename: alternate worker pathname should not exist before the test starts.\n");

  return inumber;
}

// Launch the worker race, then verify the final visible names and inode identity.
int kernel_main(void) {
  say("***ext_rename test start\n", NULL);

  ext2_init(&fs);

  barrier_init(&rename_start_barrier, RENAME_WORKERS + 1);
  barrier_init(&rename_done_barrier, RENAME_WORKERS + 1);

  for (unsigned i = 0; i < RENAME_WORKERS; ++i) {
    expected_inumbers[i] = load_worker_fixture(i);

    struct RenameWorkerArgs* args = malloc(sizeof(struct RenameWorkerArgs));
    assert(args != NULL,
      "ext_rename: failed to allocate worker args.\n");
    args->start = &rename_start_barrier;
    args->done = &rename_done_barrier;
    args->id = i;
    args->expected_inumber = expected_inumbers[i];
    args->initial_name = initial_names[i];
    args->alternate_name = alternate_names[i];
    args->expected_text = expected_texts[i];

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL,
      "ext_rename: failed to allocate worker thread metadata.\n");
    fun->func = rename_worker;
    fun->arg = args;
    thread(fun);
  }

  barrier_sync(&rename_start_barrier);
  barrier_sync(&rename_done_barrier);

  barrier_destroy(&rename_start_barrier);
  barrier_destroy(&rename_done_barrier);

  for (unsigned i = 0; i < RENAME_WORKERS; ++i) {
    assert_name_pair(i, expected_inumbers[i], expected_texts[i], RENAME_ROUNDS,
      initial_names[i], alternate_names[i]);
  }

  say("***Concurrent same-dir renames: ok\n", NULL);

  ext2_destroy(&fs);

  say("***ext_rename test complete\n", NULL);
  return 0;
}
