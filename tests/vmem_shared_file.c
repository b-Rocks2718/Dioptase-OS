/*
 * Concurrent shared file-backed mmap test.
 *
 * Validates:
 * - concurrent shared mappings of one file observe the same in-memory page
 * - a page-aligned nonzero file_offset selects the expected file page
 * - writes from different workers become visible through every live mapping
 * - the final shared bytes are written back to the backing file after unmap
 *
 * How:
 * - initialize one ext2 filesystem fixture whose second page contains the test
 *   bytes and whose first page contains different sentinel contents
 * - spawn WORKER_COUNT workers and synchronize every phase with one reusable
 *   barrier shared with the main thread
 * - each round begins with every worker mapping the file's second page by
 *   passing `file_offset = FRAME_SIZE`, then checking the bytes persisted by
 *   the previous round
 * - workers then write disjoint offsets so the final page contents are
 *   deterministic even though the writes overlap in time
 * - the main thread rereads the backing file after all workers unmap to verify
 *   that the round's shared bytes were persisted
 */

#include "../kernel/vmem.h"
#include "../kernel/ext.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"

#define WORKER_COUNT 4
#define ROUNDS 4

#define TEST_FILE_NAME "hello.txt"
// File-backed mmap offsets are page-aligned; use the second 4 KiB page so the
// first-page sentinel catches any offset-handling bug immediately.
#define TEST_FILE_OFFSET 4096
#define TEST_FILE_SIZE 4106
#define SHARED_FILE_BYTES 10
#define SHARED_BASE_TEXT "SHAREDmap\n"

struct Ext2 fs;
static struct Barrier phase_barrier;
static int finished = 0;

struct WorkerArg {
  int id;
};

static char shared_worker_byte(int id, int round) {
  return 'A' + ((round * WORKER_COUNT + id) % 26);
}

static void build_shared_expected(char* dest, int round) {
  memcpy(dest, (void*)SHARED_BASE_TEXT, SHARED_FILE_BYTES);
  if (round < 0) {
    return;
  }

  for (int id = 0; id < WORKER_COUNT; ++id) {
    dest[id] = shared_worker_byte(id, round);
  }
}

static void read_file_bytes(char* dest) {
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL,
    "vmem shared file thread: failed to reopen fixture file.\n");

  unsigned size = node_size_in_bytes(file);
  if (size != TEST_FILE_SIZE) {
    int args[2] = {(int)size, TEST_FILE_SIZE};
    say("***vmem shared file thread FAIL size=%d expected=%d\n", args);
    panic("vmem shared file thread: backing file size changed unexpectedly.\n");
  }

  unsigned cnt = node_read_all(file, TEST_FILE_OFFSET, SHARED_FILE_BYTES, dest);
  if (cnt != SHARED_FILE_BYTES) {
    int args[2] = {(int)cnt, SHARED_FILE_BYTES};
    say("***vmem shared file thread FAIL bytes=%d expected=%d\n", args);
    panic("vmem shared file thread: failed to read full backing file.\n");
  }

  node_free(file);
}

static void expect_bytes(char* got, char* expected, int worker_id,
  int round, int phase) {
  for (unsigned i = 0; i < SHARED_FILE_BYTES; ++i) {
    if (got[i] != expected[i]) {
      int args[5] = {
        worker_id,
        round,
        phase,
        (int)i,
        ((int)got[i] << 8) | (unsigned char)expected[i]
      };
      say("***vmem shared file thread FAIL id=%d round=%d phase=%d offset=%d pair=0x%X\n", args);
      panic("vmem shared file thread: byte contents mismatch.\n");
    }
  }
}

static void shared_file_worker(void* arg) {
  struct WorkerArg* worker = (struct WorkerArg*)arg;
  int id = worker->id;
  char expected[SHARED_FILE_BYTES];

  for (int round = 0; round < ROUNDS; ++round) {
    struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
    assert(file != NULL,
      "vmem shared file thread: worker failed to open fixture file.\n");

    char* mapping = mmap(SHARED_FILE_BYTES, file, TEST_FILE_OFFSET,
      MMAP_READ | MMAP_WRITE | MMAP_SHARED);
    assert(mapping != NULL,
      "vmem shared file thread: mmap returned NULL.\n");
    node_free(file);

    build_shared_expected(expected, round - 1);
    expect_bytes(mapping, expected, id, round, 0);

    barrier_sync(&phase_barrier);

    mapping[id] = shared_worker_byte(id, round);
    if (((id + round) & 1) == 1) {
      yield();
    }

    barrier_sync(&phase_barrier);

    build_shared_expected(expected, round);
    expect_bytes(mapping, expected, id, round, 1);

    barrier_sync(&phase_barrier);

    munmap(mapping);

    barrier_sync(&phase_barrier);
  }

  __atomic_fetch_add(&finished, 1);
}

void kernel_main(void) {
  say("***vmem shared file thread test start\n", NULL);

  ext2_init(&fs);
  barrier_init(&phase_barrier, WORKER_COUNT + 1);

  for (int i = 0; i < WORKER_COUNT; ++i) {
    struct WorkerArg* arg = malloc(sizeof(struct WorkerArg));
    assert(arg != NULL,
      "vmem shared file thread: failed to allocate worker args.\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL,
      "vmem shared file thread: failed to allocate thread metadata.\n");
    fun->func = shared_file_worker;
    fun->arg = arg;
    thread(fun);
  }

  char file_bytes[SHARED_FILE_BYTES];
  char expected[SHARED_FILE_BYTES];
  for (int round = 0; round < ROUNDS; ++round) {
    barrier_sync(&phase_barrier);
    barrier_sync(&phase_barrier);
    barrier_sync(&phase_barrier);
    barrier_sync(&phase_barrier);

    build_shared_expected(expected, round);
    read_file_bytes(file_bytes);
    expect_bytes(file_bytes, expected, WORKER_COUNT, round, 2);
  }

  while (__atomic_load_n(&finished) != WORKER_COUNT) {
    yield();
  }

  barrier_destroy(&phase_barrier);
  ext2_destroy(&fs);

  int args[2] = {WORKER_COUNT, ROUNDS};
  say("***vmem shared file thread ok workers=%d rounds=%d\n", args);
  say("***vmem shared file thread test complete\n", NULL);
}
