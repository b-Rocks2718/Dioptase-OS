/*
 * Covers ext2 regular-file writes: in-place overwrites, persistence after
 * reopening, file growth across block boundaries, single-indirect growth, and
 * concurrent disjoint writes to the same file.
 */
#include "../kernel/ext.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

#define CONCURRENT_WRITERS 4
#define CONCURRENT_ROUNDS 3
#define CONCURRENT_FILE_NAME "shared-concurrent.bin"

struct Ext2 fs;

struct ConcurrentWriteArgs {
  unsigned block_size;
  unsigned slot;
};

static struct Barrier concurrent_start_barrier;
static int concurrent_finished = 0;

static void concurrent_writer_thread(void* arg);

// Allocates a raw byte buffer and fills every byte with the same pattern.
// The write tests use this for file bootstrap images and per-thread payloads so
// later readback can validate exact byte ranges without string semantics.
static char* alloc_filled_bytes(unsigned size, char fill, char* failure_message) {
  char* buf = malloc(size);
  assert(buf != NULL, failure_message);

  for (unsigned i = 0; i < size; ++i){
    buf[i] = fill;
  }

  return buf;
}

// Reads the entire file payload exactly as stored on disk. The buffer is not
// NUL-terminated because several tests intentionally validate zero-filled gaps.
static char* read_node_bytes(struct Node* node) {
  unsigned size = node_size_in_bytes(node);
  char* buf = malloc(size);
  assert(size == 0 || buf != NULL, "ext_write: failed to allocate a read buffer.\n");

  if (size > 0){
    unsigned cnt = node_read_all(node, 0, size, buf);
    assert(cnt == size, "ext_write: failed to read the full file contents.\n");
  }

  return buf;
}

// Verifies that a byte window contains one repeated value. This keeps the
// failure messages focused on the semantic contract being tested instead of the
// mechanics of the byte loop.
static void assert_region_is_byte(char* buf, unsigned start, unsigned size,
  char expected, char* failure_message) {
  int mismatch = 0;

  for (unsigned i = 0; i < size; ++i){
    if (buf[start + i] != expected){
      mismatch = 1;
    }
  }

  assert(mismatch == 0, failure_message);
}

// Verifies that a short expected string appears at an exact byte offset within
// a larger file image without requiring the surrounding file to be text.
static void assert_text_at(char* buf, unsigned offset, char* expected,
  char* failure_message) {
  unsigned expected_len = strlen(expected);
  int mismatch = 0;

  for (unsigned i = 0; i < expected_len; ++i){
    if (buf[offset + i] != expected[i]){
      mismatch = 1;
    }
  }

  assert(mismatch == 0, failure_message);
}

// Covers writes to an existing small file. The first write stays within the old
// file size; the second grows the file and then reopens it to prove the inode
// size and data blocks were both updated on disk.
static void check_existing_file_writes(struct Node* root) {
  struct Node* hello = node_find(root, "hello.txt");
  assert(hello != NULL, "ext_write: hello.txt should exist in the root directory.\n");

  unsigned cnt = node_write_all(hello, 0, 3, "abc");
  assert(cnt == 3, "ext_write: in-place overwrite returned the wrong byte count.\n");
  assert(node_size_in_bytes(hello) == 5,
    "ext_write: in-place overwrite should not change the file size.\n");
  node_free(hello);

  hello = node_find(root, "hello.txt");
  assert(hello != NULL, "ext_write: failed to reopen hello.txt after an overwrite.\n");
  char* hello_bytes = read_node_bytes(hello);
  assert_text_at(hello_bytes, 0, "abclo",
    "ext_write: in-place overwrite did not persist the expected bytes.\n");
  free(hello_bytes);
  node_free(hello);

  hello = node_find(root, "hello.txt");
  assert(hello != NULL, "ext_write: failed to reopen hello.txt for growth.\n");
  cnt = node_write_all(hello, 0, 10, "abcdefghij");
  assert(cnt == 10, "ext_write: overwrite growth returned the wrong byte count.\n");
  assert(node_size_in_bytes(hello) == 10,
    "ext_write: overwrite growth should update the file size.\n");
  node_free(hello);

  hello = node_find(root, "hello.txt");
  assert(hello != NULL, "ext_write: failed to reopen hello.txt after growth.\n");
  hello_bytes = read_node_bytes(hello);
  assert_text_at(hello_bytes, 0, "abcdefghij",
    "ext_write: overwrite growth did not persist the full new contents.\n");
  free(hello_bytes);
  node_free(hello);

  say("***Existing file writes: ok\n", NULL);
}

// Covers the common "create empty file, then write initial contents" path and
// verifies that reopening the inode sees the same bytes.
static void check_new_file_growth(struct Node* root) {
  struct Node* new_file = node_make_file(root, "new_file.txt");
  assert(new_file != NULL, "ext_write: failed to create new_file.txt.\n");

  unsigned cnt = node_write_all(new_file, 0, 12, "Hello World!");
  assert(cnt == 12, "ext_write: new file write returned the wrong byte count.\n");
  assert(node_size_in_bytes(new_file) == 12,
    "ext_write: new file write should set the file size.\n");
  node_free(new_file);

  new_file = node_find(root, "new_file.txt");
  assert(new_file != NULL, "ext_write: failed to reopen new_file.txt after writing it.\n");
  char* bytes = read_node_bytes(new_file);
  assert_text_at(bytes, 0, "Hello World!",
    "ext_write: new file write did not persist the expected contents.\n");
  free(bytes);
  node_free(new_file);

  say("***New file write persistence: ok\n", NULL);
}

// Covers concurrent writes to the same inode while avoiding overwrite races in
// the expected data: each worker owns one whole ext2 block-sized region in a
// pre-sized file. This stresses cache and inode writeback interleavings without
// making the expected final contents scheduler-dependent.
static void check_concurrent_disjoint_writes(struct Node* root, unsigned block_size) {
  struct Node* shared = node_make_file(root, CONCURRENT_FILE_NAME);
  assert(shared != NULL, "ext_write: failed to create the concurrent write test file.\n");

  unsigned total_size = CONCURRENT_WRITERS * block_size;
  char* zeroes = alloc_filled_bytes(total_size, 0,
    "ext_write: failed to allocate the concurrent file bootstrap buffer.\n");
  unsigned cnt = node_write_all(shared, 0, total_size, zeroes);
  assert(cnt == total_size,
    "ext_write: failed to pre-size the concurrent write test file.\n");
  free(zeroes);
  node_free(shared);

  concurrent_finished = 0;
  barrier_init(&concurrent_start_barrier, CONCURRENT_WRITERS + 1);

  for (unsigned i = 0; i < CONCURRENT_WRITERS; ++i){
    struct ConcurrentWriteArgs* args = malloc(sizeof(struct ConcurrentWriteArgs));
    assert(args != NULL, "ext_write: concurrent writer args allocation failed.\n");
    args->block_size = block_size;
    args->slot = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "ext_write: concurrent writer Fun allocation failed.\n");
    fun->func = concurrent_writer_thread;
    fun->arg = args;

    thread(fun);
  }

  barrier_sync(&concurrent_start_barrier);

  while (__atomic_load_n(&concurrent_finished) != CONCURRENT_WRITERS) {
    yield();
  }

  barrier_destroy(&concurrent_start_barrier);

  shared = node_find(root, CONCURRENT_FILE_NAME);
  assert(shared != NULL, "ext_write: failed to reopen the concurrent write test file.\n");
  assert(node_size_in_bytes(shared) == total_size,
    "ext_write: concurrent disjoint writes changed the file size unexpectedly.\n");

  char* bytes = read_node_bytes(shared);
  for (unsigned i = 0; i < CONCURRENT_WRITERS; ++i){
    assert_region_is_byte(bytes, i * block_size, block_size, 'A' + i,
      "ext_write: concurrent disjoint writes lost or corrupted one writer's block.\n");
  }
  free(bytes);
  node_free(shared);

  say("***Concurrent disjoint writes: ok\n", NULL);
}

// Covers file growth where the first write starts after offset 0. ext2 should
// expose the unwritten prefix as zero bytes and persist the payload at the
// requested later offset.
static void check_gap_zero_fill(struct Node* root) {
  struct Node* gap = node_make_file(root, "gap-file.bin");
  assert(gap != NULL, "ext_write: failed to create gap-file.bin.\n");

  unsigned cnt = node_write_all(gap, 4, 3, "XYZ");
  assert(cnt == 3, "ext_write: gap write returned the wrong byte count.\n");
  assert(node_size_in_bytes(gap) == 7,
    "ext_write: gap write should extend the file to offset + size.\n");
  node_free(gap);

  gap = node_find(root, "gap-file.bin");
  assert(gap != NULL, "ext_write: failed to reopen gap-file.bin.\n");
  char* bytes = read_node_bytes(gap);
  assert_region_is_byte(bytes, 0, 4, 0,
    "ext_write: bytes between the old EOF and the first write offset were not zero-filled.\n");
  assert_text_at(bytes, 4, "XYZ",
    "ext_write: gap write did not persist the trailing payload.\n");
  free(bytes);
  node_free(gap);

  say("***Gap zero fill: ok\n", NULL);
}

// Covers a payload that begins near the end of one logical block and continues
// into the next. That exercises both block allocation and partial-block writes
// on each side of the boundary.
static void check_cross_block_growth(struct Node* root, unsigned block_size) {
  struct Node* cross = node_make_file(root, "cross-block.bin");
  assert(cross != NULL, "ext_write: failed to create cross-block.bin.\n");

  unsigned offset = block_size - 2;
  unsigned cnt = node_write_all(cross, offset, 6, "UVWXYZ");
  assert(cnt == 6, "ext_write: cross-block write returned the wrong byte count.\n");
  assert(node_size_in_bytes(cross) == block_size + 4,
    "ext_write: cross-block write should extend the file across the boundary.\n");
  node_free(cross);

  cross = node_find(root, "cross-block.bin");
  assert(cross != NULL, "ext_write: failed to reopen cross-block.bin.\n");
  char* bytes = read_node_bytes(cross);
  assert_region_is_byte(bytes, offset - 2, 2, 0,
    "ext_write: cross-block growth did not preserve zeroes before the first payload bytes.\n");
  assert_text_at(bytes, offset, "UVWXYZ",
    "ext_write: cross-block write did not persist the expected payload.\n");
  free(bytes);
  node_free(cross);

  say("***Cross-block growth: ok\n", NULL);
}

// Covers the first write that must use the inode's single-indirect block. The
// readback window includes the three bytes before the payload so the test also
// proves the final gap bytes remained zero-filled.
static void check_single_indirect_growth(struct Node* root, unsigned block_size) {
  struct Node* indirect = node_make_file(root, "indirect.bin");
  assert(indirect != NULL, "ext_write: failed to create indirect.bin.\n");

  unsigned offset = block_size * 13 + 3;
  unsigned cnt = node_write_all(indirect, offset, 8, "INDIRECT");
  assert(cnt == 8, "ext_write: single-indirect growth returned the wrong byte count.\n");
  assert(node_size_in_bytes(indirect) == offset + 8,
    "ext_write: single-indirect growth should extend the file to the end of the payload.\n");
  node_free(indirect);

  indirect = node_find(root, "indirect.bin");
  assert(indirect != NULL, "ext_write: failed to reopen indirect.bin.\n");

  char* window = malloc(11);
  assert(window != NULL, "ext_write: failed to allocate the indirect read window.\n");
  cnt = node_read_all(indirect, offset - 3, 11, window);
  assert(cnt == 11, "ext_write: failed to read back the indirect write window.\n");
  assert_region_is_byte(window, 0, 3, 0,
    "ext_write: bytes before the single-indirect payload were not zero-filled.\n");
  assert_text_at(window, 3, "INDIRECT",
    "ext_write: single-indirect growth did not persist the expected payload.\n");
  free(window);
  node_free(indirect);

  say("***Single-indirect growth: ok\n", NULL);
}

// Each worker reopens the shared file so concurrent writes exercise the same
// lookup and inode-cache paths a real caller would use. Workers all start
// together after the barrier and repeatedly rewrite their own disjoint region.
static void concurrent_writer_thread(void* arg) {
  struct ConcurrentWriteArgs* write_args = (struct ConcurrentWriteArgs*)arg;
  struct Node* file = node_find(&fs.root, CONCURRENT_FILE_NAME);
  assert(file != NULL, "ext_write: concurrent writer failed to reopen the shared file.\n");

  char* block = alloc_filled_bytes(write_args->block_size, 'A' + write_args->slot,
    "ext_write: failed to allocate a concurrent writer payload block.\n");

  barrier_sync(&concurrent_start_barrier);

  for (unsigned round = 0; round < CONCURRENT_ROUNDS; ++round) {
    unsigned cnt = node_write_all(file, write_args->slot * write_args->block_size,
      write_args->block_size, block);
    assert(cnt == write_args->block_size,
      "ext_write: concurrent writer returned the wrong byte count.\n");
  }

  free(block);
  node_free(file);
  __atomic_fetch_add(&concurrent_finished, 1);
}

int kernel_main(void) {
  // The suite progresses from small sequential writes to larger growth cases,
  // then finishes with the first indirect-block case that previously triggered
  // block-0 I/O warnings in the emulator.
  say("***Hello from ext2 write test!\n", NULL);

  ext2_init(&fs);

  struct Node* root = &fs.root;
  unsigned block_size = ext2_get_block_size(&fs);

  check_existing_file_writes(root);
  check_new_file_growth(root);
  check_concurrent_disjoint_writes(root, block_size);
  check_gap_zero_fill(root);
  check_cross_block_growth(root, block_size);
  check_single_indirect_growth(root, block_size);

  ext2_destroy(&fs);
  return 0;
}
