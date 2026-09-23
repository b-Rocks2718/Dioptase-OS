/*
 * Concurrent shared file-backed mmap test.
 *
 * Validates:
 * - concurrent shared mappings of one file observe the same in-memory page
 * - a page-aligned nonzero file_offset selects the expected file page
 * - writes from different workers become visible through every live mapping
 * - the final shared bytes are written back to the backing file after unmap
 * - later writable aliases max-merge their exposed writeback extent, while a
 *   wider read-only alias cannot extend a dirty writer's file
 * - truncate caps partial-page dirty writeback and discards dirty pages wholly
 *   beyond its new EOF, without preventing a later writable fault from
 *   deliberately extending the file again
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
 * - after joining the workers, the main thread uses isolated empty files and
 *   deliberate fault order to make each extent/truncate outcome deterministic
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
#define SHARED_FILE_BYTES 10
#define SHARED_BASE_TEXT "SHAREDmap\n"

#define EXTENT_MERGE_FILE_NAME "extent-merge.bin"
#define READ_ONLY_EXTENT_FILE_NAME "readonly-extent.bin"
#define TRUNCATE_CACHE_FILE_NAME "truncate-cache.bin"

#define NARROW_EXTENT_BYTES 1
#define WIDE_EXTENT_BYTES 4
#define TRUNCATE_INITIAL_BYTES 8

static struct Barrier phase_barrier;
static int finished = 0;

struct WorkerArg { /* Identifies one writer to the shared file-backed mapping. */
  int id;
};

static char shared_worker_byte(int id, int round) { /* Produce the byte each shared-mapping worker writes for this round. */
  return 'A' + ((round * WORKER_COUNT + id) % 26);
}

static void build_shared_expected(char* dest, int round) { /* Reconstruct the backing bytes expected after all writers complete a round. */
  memcpy(dest, (void*)SHARED_BASE_TEXT, SHARED_FILE_BYTES);
  if (round < 0) {
    return;
  }

  for (int id = 0; id < WORKER_COUNT; ++id) {
    dest[id] = shared_worker_byte(id, round);
  }
}

static void read_file_bytes(char* dest) { /* Read the complete mapped fixture range directly from its backing node. */
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL,
    "vmem shared file thread: failed to reopen fixture file.\n");

  unsigned size = node_size_in_bytes(file);
  if (size != TEST_FILE_OFFSET + SHARED_FILE_BYTES) {
    int args[2] = {(int)size, TEST_FILE_OFFSET + SHARED_FILE_BYTES};
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

static void expect_bytes(char* got, char* expected, int worker_id, /* Compare a worker's mapped view with phase-specific expected bytes. */
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

// The first cache miss used to permanently choose the eventual dirty
// writeback size. Fault the one-byte writable mapping first, then fault a
// four-byte writable alias and change its final byte. The later writable
// exposure must enlarge the serialized writeback extent to four bytes.
static void check_writable_extent_merge(void){
  struct Node* file = node_make_file(&fs.root, EXTENT_MERGE_FILE_NAME);
  assert(file != NULL,
    "vmem shared file: failed to create writable extent fixture.\n");

  char* narrow = mmap(NARROW_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(narrow != NULL,
    "vmem shared file: failed to map narrow writable alias.\n");
  narrow[0] = 'N';

  char* wider = mmap(WIDE_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(wider != NULL,
    "vmem shared file: failed to map wider writable alias.\n");
  wider[WIDE_EXTENT_BYTES - 1] = 'W';

  munmap(narrow);
  munmap(wider);

  assert(node_size_in_bytes(file) == WIDE_EXTENT_BYTES,
    "vmem shared file: wider writable alias did not persist its file extent.\n");
  char persisted[WIDE_EXTENT_BYTES];
  unsigned cnt = node_read_all(file, 0, sizeof(persisted), persisted);
  assert(cnt == sizeof(persisted),
    "vmem shared file: writable extent fixture read was short.\n");
  assert(persisted[0] == 'N' && persisted[WIDE_EXTENT_BYTES - 1] == 'W',
    "vmem shared file: later writable alias byte was not persisted.\n");

  node_free(file);
}

// A tempting fix is to max-merge every cache acquisition. That would make a
// four-byte read-only alias enlarge a one-byte dirty writer. Keep the read-only
// mapping live until final release so this regression exercises exactly that
// erroneous final-writeback path.
static void check_read_only_extent_is_inert(void){
  struct Node* file = node_make_file(&fs.root, READ_ONLY_EXTENT_FILE_NAME);
  assert(file != NULL,
    "vmem shared file: failed to create read-only extent fixture.\n");

  char* writable = mmap(NARROW_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(writable != NULL,
    "vmem shared file: failed to map narrow dirty writer.\n");
  writable[0] = 'D';

  char* read_only = mmap(WIDE_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_SHARED);
  assert(read_only != NULL,
    "vmem shared file: failed to map wider read-only alias.\n");
  char tail = read_only[WIDE_EXTENT_BYTES - 1];
  assert(tail == 0,
    "vmem shared file: zero-filled read-only cache tail was not zero.\n");

  munmap(writable);
  munmap(read_only);

  assert(node_size_in_bytes(file) == NARROW_EXTENT_BYTES,
    "vmem shared file: read-only alias enlarged dirty writeback extent.\n");
  char persisted = 0;
  unsigned cnt = node_read_all(file, 0, NARROW_EXTENT_BYTES, &persisted);
  assert(cnt == NARROW_EXTENT_BYTES && persisted == 'D',
    "vmem shared file: narrow dirty writer did not persist exactly one byte.\n");

  node_free(file);
}

/*
 * Dirty both a page straddling the new EOF and a page wholly beyond it, then
 * truncate while both cache entries remain referenced. Serialized truncation
 * must retain the first dirty byte, cap the first page at one byte, and clear
 * the second page's dirty state. Once both old mappings are gone, a new
 * writable fault is intentionally allowed to publish a four-byte extent and
 * extend the file again under the existing shared-mmap contract.
 */
static void check_truncate_cache_serialization(void){
  struct Node* file = node_make_file(&fs.root, TRUNCATE_CACHE_FILE_NAME);
  assert(file != NULL,
    "vmem shared file: failed to create truncate cache fixture.\n");

  unsigned cnt = node_write_all(file, 0, TRUNCATE_INITIAL_BYTES, "abcdefgh");
  assert(cnt == TRUNCATE_INITIAL_BYTES,
    "vmem shared file: failed to initialize truncate cache fixture.\n");

  char* partial = mmap(TRUNCATE_INITIAL_BYTES, file, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(partial != NULL,
    "vmem shared file: failed to map truncate partial page.\n");
  partial[0] = 'P';
  partial[TRUNCATE_INITIAL_BYTES - 1] = 'X';

  char* beyond = mmap(WIDE_EXTENT_BYTES, file, TEST_FILE_OFFSET,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(beyond != NULL,
    "vmem shared file: failed to map truncate beyond-EOF page.\n");
  beyond[0] = 'B';

  assert(vmem_truncate_file(file, NARROW_EXTENT_BYTES),
    "vmem shared file: serialized truncate rejected a valid shrink.\n");

  munmap(beyond);
  munmap(partial);

  assert(node_size_in_bytes(file) == NARROW_EXTENT_BYTES,
    "vmem shared file: dirty release restored bytes beyond truncated EOF.\n");
  char prefix = 0;
  cnt = node_read_all(file, 0, NARROW_EXTENT_BYTES, &prefix);
  assert(cnt == NARROW_EXTENT_BYTES && prefix == 'P',
    "vmem shared file: truncate discarded dirty data before the new EOF.\n");

  // Retain a read-only reference so the next writable fault republishes its
  // extent into an already-live post-truncate cache entry, not merely a fresh
  // cache miss.
  char* retained_read = mmap(NARROW_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_SHARED);
  assert(retained_read != NULL,
    "vmem shared file: failed to retain post-truncate cache entry.\n");
  char retained_prefix = retained_read[0];
  assert(retained_prefix == 'P',
    "vmem shared file: retained post-truncate prefix was incorrect.\n");

  char* later = mmap(WIDE_EXTENT_BYTES, file, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(later != NULL,
    "vmem shared file: failed to map post-truncate writable alias.\n");
  later[WIDE_EXTENT_BYTES - 1] = 'E';
  munmap(later);

  assert(node_size_in_bytes(file) == NARROW_EXTENT_BYTES,
    "vmem shared file: non-final writable release wrote through live cache entry.\n");
  munmap(retained_read);

  assert(node_size_in_bytes(file) == WIDE_EXTENT_BYTES,
    "vmem shared file: later writable fault did not extend truncated file.\n");
  char regrown[WIDE_EXTENT_BYTES];
  cnt = node_read_all(file, 0, sizeof(regrown), regrown);
  assert(cnt == sizeof(regrown) && regrown[0] == 'P' &&
      regrown[WIDE_EXTENT_BYTES - 1] == 'E',
    "vmem shared file: post-truncate shared-mmap bytes did not persist.\n");

  node_free(file);
}

static void shared_file_worker(void* arg) { /* Update one byte through a shared mapping and verify cross-worker visibility each round. */
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

void kernel_main(void) { /* Verify shared file mappings observe cross-thread writes. */
  say("***vmem shared file thread test start\n", NULL);

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

  check_writable_extent_merge();
  check_read_only_extent_is_inert();
  check_truncate_cache_serialization();

  say("***vmem shared file cache extent/truncate: ok\n", NULL);

  int args[2] = {WORKER_COUNT, ROUNDS};
  say("***vmem shared file thread ok workers=%d rounds=%d\n", args);
  say("***vmem shared file thread test complete\n", NULL);
}
