/*
 * Concurrent private file-backed mmap test.
 *
 * Validates:
 * - concurrent private mappings of one file fault in the original file bytes
 * - a page-aligned nonzero file_offset selects the expected file page
 * - each worker's writes stay private to that mapping and do not leak into
 *   other workers' mappings
 * - the backing file remains unchanged both while private mappings are live and
 *   after those mappings are unmapped
 *
 * How:
 * - initialize one ext2 filesystem fixture whose second page contains the test
 *   bytes and whose first page contains different sentinel contents
 * - spawn WORKER_COUNT workers and reuse one barrier with the main thread
 * - in each round every worker privately maps the second page by passing
 *   `file_offset = FRAME_SIZE`, then drops the original node wrapper so the
 *   VME must keep its own reference alive
 * - after a synchronized start, each worker verifies the baseline bytes and
 *   overwrites one worker-owned offset
 * - the main thread rereads the backing file while the mappings are still live,
 *   then again after unmap, to prove private writes never reach disk
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
#define TEST_FILE_SIZE 4107
#define PRIVATE_FILE_BYTES 11
#define PRIVATE_BASE_TEXT "PRIVATEmap\n"

static struct Barrier phase_barrier;
static int finished = 0;

struct WorkerArg {
  int id;
};

static char private_worker_byte(int id, int round) {
  return 'a' + ((round * WORKER_COUNT + id) % 26);
}

static void read_file_bytes(char* dest) {
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL,
    "vmem private file thread: failed to reopen fixture file.\n");

  unsigned size = node_size_in_bytes(file);
  if (size != TEST_FILE_SIZE) {
    int args[2] = {(int)size, TEST_FILE_SIZE};
    say("***vmem private file thread FAIL size=%d expected=%d\n", args);
    panic("vmem private file thread: backing file size changed unexpectedly.\n");
  }

  unsigned cnt = node_read_all(file, TEST_FILE_OFFSET, PRIVATE_FILE_BYTES, dest);
  if (cnt != PRIVATE_FILE_BYTES) {
    int args[2] = {(int)cnt, PRIVATE_FILE_BYTES};
    say("***vmem private file thread FAIL bytes=%d expected=%d\n", args);
    panic("vmem private file thread: failed to read full backing file.\n");
  }

  node_free(file);
}

static void expect_bytes(char* got, char* expected, int worker_id,
  int round, int phase) {
  for (unsigned i = 0; i < PRIVATE_FILE_BYTES; ++i) {
    if (got[i] != expected[i]) {
      int args[5] = {
        worker_id,
        round,
        phase,
        (int)i,
        ((int)got[i] << 8) | (unsigned char)expected[i]
      };
      say("***vmem private file thread FAIL id=%d round=%d phase=%d offset=%d pair=0x%X\n", args);
      panic("vmem private file thread: byte contents mismatch.\n");
    }
  }
}

static void build_private_expected(char* dest, int worker_id, int round) {
  memcpy(dest, (void*)PRIVATE_BASE_TEXT, PRIVATE_FILE_BYTES);
  dest[worker_id] = private_worker_byte(worker_id, round);
}

static void private_file_worker(void* arg) {
  struct WorkerArg* worker = (struct WorkerArg*)arg;
  int id = worker->id;
  char expected[PRIVATE_FILE_BYTES];

  for (int round = 0; round < ROUNDS; ++round) {
    struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
    assert(file != NULL,
      "vmem private file thread: worker failed to open fixture file.\n");

    char* mapping = mmap(PRIVATE_FILE_BYTES, file, TEST_FILE_OFFSET,
      MMAP_READ | MMAP_WRITE);
    assert(mapping != NULL,
      "vmem private file thread: mmap returned NULL.\n");
    node_free(file);

    memcpy(expected, (void*)PRIVATE_BASE_TEXT, PRIVATE_FILE_BYTES);
    expect_bytes(mapping, expected, id, round, 0);

    barrier_sync(&phase_barrier);

    mapping[id] = private_worker_byte(id, round);
    if (((id + round) & 1) == 0) {
      yield();
    }

    barrier_sync(&phase_barrier);

    build_private_expected(expected, id, round);
    expect_bytes(mapping, expected, id, round, 1);

    barrier_sync(&phase_barrier);

    munmap(mapping);

    barrier_sync(&phase_barrier);
  }

  __atomic_fetch_add(&finished, 1);
}

void kernel_main(void) {
  say("***vmem private file thread test start\n", NULL);

  barrier_init(&phase_barrier, WORKER_COUNT + 1);

  for (int i = 0; i < WORKER_COUNT; ++i) {
    struct WorkerArg* arg = malloc(sizeof(struct WorkerArg));
    assert(arg != NULL,
      "vmem private file thread: failed to allocate worker args.\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL,
      "vmem private file thread: failed to allocate thread metadata.\n");
    fun->func = private_file_worker;
    fun->arg = arg;
    thread(fun);
  }

  char file_bytes[PRIVATE_FILE_BYTES];
  for (int round = 0; round < ROUNDS; ++round) {
    barrier_sync(&phase_barrier);
    barrier_sync(&phase_barrier);

    read_file_bytes(file_bytes);
    expect_bytes(file_bytes, (char*)PRIVATE_BASE_TEXT, WORKER_COUNT, round, 2);

    barrier_sync(&phase_barrier);
    barrier_sync(&phase_barrier);

    read_file_bytes(file_bytes);
    expect_bytes(file_bytes, (char*)PRIVATE_BASE_TEXT, WORKER_COUNT, round, 3);
  }

  while (__atomic_load_n(&finished) != WORKER_COUNT) {
    yield();
  }

  barrier_destroy(&phase_barrier);

  int args[2] = {WORKER_COUNT, ROUNDS};
  say("***vmem private file thread ok workers=%d rounds=%d\n", args);
  say("***vmem private file thread test complete\n", NULL);
}
