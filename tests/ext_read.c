/*
 * ext2 read test.
 *
 * Validates:
 * - file, directory, and symlink lookups decode the right inode type and data
 * - path traversal handles nested paths, absolute paths, empty paths, and
 *   missing paths correctly
 * - read helpers honor EOF and preserve cross-block marker positions
 * - concurrent readers can share inode-cache and block-cache state safely
 *
 * How:
 * - open the root fixtures from tests/ext_read.dir and validate their metadata,
 *   contents, and traversal behavior one helper at a time
 * - locate the marker inside blocks.txt and re-read both the direct block and a
 *   smaller marker window to prove offset handling stays correct
 * - exercise EOF-clamped reads and missing lookups
 * - launch a short concurrent phase where several readers hit hello.txt and
 *   blocks.txt together through the shared caches
 */
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

#define EXT2_BLOCK_SIZE_1K 1024
#define EXT2_BLOCK_SIZE_2K 2048
#define EXT2_BLOCK_SIZE_4K 4096
#define CONCURRENT_READERS 8
#define CONCURRENT_ROUNDS 3
#define MARKER_WINDOW_LEAD_BYTES 32
#define MARKER_WINDOW_MAX_BYTES 320
#define BLOCKS_MARKER "BLOCK1-MARKER\n"
#define FILE_END_MARKER "FILE-END\n"

struct ConcurrentReadArgs {
  unsigned block_size;
};

static struct Barrier concurrent_start_barrier;
static int concurrent_finished = 0;
static int concurrent_hello_reads = 0;
static int concurrent_block_reads = 0;

// Chooses a small read window that always covers BLOCK1-MARKER. When the
// marker lives in a later logical block, the window starts just before that
// block boundary so the read crosses the same boundary the full test cares
// about. When the file fits in one block, the window falls back to a local
// marker-centered slice.
static void choose_marker_window(unsigned marker_offset, unsigned file_size,
  unsigned block_size, unsigned* window_offset, unsigned* window_size) {
  unsigned marker_block = marker_offset / block_size;

  if (marker_block > 0) {
    *window_offset = marker_block * block_size - MARKER_WINDOW_LEAD_BYTES;
  } else if (marker_offset > MARKER_WINDOW_LEAD_BYTES) {
    *window_offset = marker_offset - MARKER_WINDOW_LEAD_BYTES;
  } else {
    *window_offset = 0;
  }

  *window_size = MARKER_WINDOW_MAX_BYTES;
  if (*window_offset + *window_size > file_size) {
    *window_size = file_size - *window_offset;
  }
}

// Verifies the ownership contract for root lookups: node_find("/") should
// return a heap-owned wrapper that callers can release with node_free().
static void check_root_lookup_contract(struct Node* root) {
  struct Node* looked_up_root = node_find(root, "/");
  assert(looked_up_root != NULL,
    "ext_read: root lookup should return a valid node.\n");
  assert(node_is_dir(looked_up_root),
    "ext_read: root lookup should decode as a directory.\n");
  assert(looked_up_root->cached->inumber == root->cached->inumber,
    "ext_read: root lookup should resolve to the root inode number.\n");
  assert(looked_up_root != root,
    "ext_read: node_find should return a heap wrapper for root lookups.\n");
  assert(looked_up_root->cached == root->cached,
    "ext_read: should only be one copy in cache\n");

  node_free(looked_up_root);
  say("***Root lookup ownership: ok\n", NULL);
}

// Empty-path lookups should preserve the same ownership contract as other
// successful lookups: callers receive a fresh heap wrapper that aliases the
// same cached inode as the starting directory.
static void check_empty_path_lookup_contract(struct Node* dir) {
  struct Node* same_dir = node_find(dir, "");
  assert(same_dir != NULL,
    "ext_read: empty-path lookup should return the starting directory.\n");
  assert(node_is_dir(same_dir),
    "ext_read: empty-path lookup from a directory should still return a directory.\n");
  assert(same_dir->cached->inumber == dir->cached->inumber,
    "ext_read: empty-path lookup should preserve inode identity.\n");
  assert(same_dir != dir,
    "ext_read: empty-path lookup should return a heap-owned wrapper.\n");
  assert(same_dir->cached == dir->cached,
    "ext_read: empty-path lookup should reuse the cached inode entry.\n");

  node_free(same_dir);
  say("***Empty path lookup: ok\n", NULL);
}

// Reads the full logical file into a heap buffer and appends a trailing NUL so
// test assertions can safely treat the result as text.
static char* read_node_text(struct Node* node) {
  unsigned size = node_size_in_bytes(node);
  char* text = malloc(size + 1);
  node_read_all(node, 0, size, text);
  text[size] = 0;
  return text;
}

// Finds the first occurrence of a marker inside raw file data. This lets the
// test prove that a later marker really lives in a later disk block without
// depending on the marker starting at an exact byte offset.
static int find_marker(char* data, unsigned data_size, char* marker) {
  unsigned marker_size = strlen(marker);

  if (marker_size == 0 || data_size < marker_size) {
    return -1;
  }

  for (unsigned i = 0; i <= data_size - marker_size; ++i) {
    if (strneq(data + i, marker, marker_size)) {
      return i;
    }
  }

  return -1;
}

// Verifies a simple one-block file using both metadata decoding and a direct
// block read. The returned inode number is reused later to confirm that an
// absolute path resets back to the filesystem root.
static unsigned check_hello_file(struct Node* root, unsigned block_size) {
  struct Node* hello = node_find(root, "hello.txt");
  struct Node* hello_again = node_find(root, "hello.txt");
  assert(hello != NULL, "ext_read: hello.txt not found in root directory.\n");
  assert(node_is_file(hello), "ext_read: hello.txt should decode as a regular file.\n");
  assert(node_get_type(hello) == EXT2_S_IFREG,
    "ext_read: hello.txt inode type did not decode as a regular file.\n");
  assert(hello->cached == hello_again->cached,
    "ext_read: repeated lookups of the same file should return wrappers that share the same cache entry.\n");

  char* hello_block = malloc(block_size + 1);
  node_read_block(hello, 0, hello_block);
  hello_block[node_size_in_bytes(hello)] = 0;
  assert(streq(hello_block, "Hello!"),
    "ext_read: node_read_block did not return the expected hello.txt contents.\n");

  int hello_args[1] = { (int)hello_block };
  say("***Hello content: %s\n", hello_args);

  unsigned hello_inumber = hello->cached->inumber;

  free(hello_block);
  node_free(hello);
  node_free(hello_again);

  return hello_inumber;
}

// Exercises directory metadata, multi-component path traversal, lookup from a
// non-root directory, absolute-path reset back to the filesystem root, and
// symlink target decoding.
static void check_nested_entries(struct Node* root, unsigned hello_inumber) {
  struct Node* nested = node_find(root, "nested");
  assert(nested != NULL, "ext_read: nested directory not found in root directory.\n");
  assert(node_is_dir(nested), "ext_read: nested should decode as a directory.\n");

  unsigned nested_entries = node_entry_count(nested);
  assert(nested_entries == 3,
    "ext_read: nested should contain '.', '..', and inner.txt only.\n");
  assert(node_get_num_links(nested) == 2,
    "ext_read: nested should have exactly two directory links.\n");
  say("***Nested metadata: ok\n", NULL);
  check_empty_path_lookup_contract(nested);

  struct Node* nested_file = node_find(nested, "inner.txt");
  assert(nested_file != NULL,
    "ext_read: nested directory lookup for inner.txt failed.\n");
  assert(node_is_file(nested_file),
    "ext_read: nested/inner.txt should decode as a regular file.\n");

  char* nested_text = read_node_text(nested_file);
  assert(streq(nested_text, "Nested hello\n"),
    "ext_read: node_read_all returned the wrong nested file contents.\n");
  say("***Nested content: ok\n", NULL);

  // Resolve the same file through two multi-component paths. This covers
  // slash-separated traversal and symlink expansion within the traversal loop.
  // It also ensures intermediate path nodes are released instead of leaking.
  struct Node* nested_file_path = node_find(root, "nested/inner.txt");
  assert(nested_file_path != NULL,
    "ext_read: nested/inner.txt multi-component lookup failed.\n");
  assert(node_is_file(nested_file_path),
    "ext_read: nested/inner.txt path lookup should return a regular file.\n");

  char* nested_path_text = read_node_text(nested_file_path);
  assert(streq(nested_path_text, "Nested hello\n"),
    "ext_read: multi-component nested/inner.txt returned the wrong contents.\n");

  struct Node* nested_link_path = node_find(root, "nested.link/inner.txt");
  assert(nested_link_path != NULL,
    "ext_read: nested.link/inner.txt symlink traversal lookup failed.\n");
  assert(node_is_file(nested_link_path),
    "ext_read: nested.link/inner.txt should resolve to a regular file.\n");

  char* nested_link_text = read_node_text(nested_link_path);
  assert(streq(nested_link_text, "Nested hello\n"),
    "ext_read: symlink-expanded multi-component lookup returned the wrong contents.\n");

  struct Node* dotted_nested_link_path = node_find(root, "dotted-nested.link/inner.txt");
  assert(dotted_nested_link_path != NULL,
    "ext_read: ./nested symlink traversal lookup failed.\n");
  assert(node_is_file(dotted_nested_link_path),
    "ext_read: dotted-nested.link/inner.txt should resolve to a regular file.\n");

  char* dotted_nested_link_text = read_node_text(dotted_nested_link_path);
  assert(streq(dotted_nested_link_text, "Nested hello\n"),
    "ext_read: dotted relative symlink target expanded to the wrong contents.\n");

  struct Node* absolute_nested_link_path = node_find(root, "abs-nested.link/inner.txt");
  assert(absolute_nested_link_path != NULL,
    "ext_read: /nested symlink traversal lookup failed.\n");
  assert(node_is_file(absolute_nested_link_path),
    "ext_read: abs-nested.link/inner.txt should resolve to a regular file.\n");

  char* absolute_nested_link_text = read_node_text(absolute_nested_link_path);
  assert(streq(absolute_nested_link_text, "Nested hello\n"),
    "ext_read: absolute symlink target expanded to the wrong contents.\n");
  say("***Multi-part paths: ok\n", NULL);

  struct Node* absolute_hello = node_find(nested, "/hello.txt");
  assert(absolute_hello != NULL,
    "ext_read: absolute path lookup should restart from the ext2 root.\n");
  assert(absolute_hello->cached->inumber == hello_inumber,
    "ext_read: /hello.txt should resolve to the same inode as hello.txt.\n");

  struct Node* nested_link = node_find(root, "nested.link");
  assert(nested_link != NULL, "ext_read: nested.link symlink not found.\n");
  assert(node_is_symlink(nested_link),
    "ext_read: nested.link should decode as a symbolic link.\n");

  char* nested_target = malloc(node_size_in_bytes(nested_link) + 1);
  node_get_symlink_target(nested_link, nested_target);
  assert(streq(nested_target, "nested"),
    "ext_read: nested.link should point at the nested directory.\n");

  // Start a lookup from the symlink node itself. This is the case that needs
  // the symlink's containing directory metadata: the relative target must be
  // resolved from the directory that contains `nested.link`, not from the
  // symlink inode wrapper.
  struct Node* from_symlink = node_find(nested_link, "inner.txt");
  assert(from_symlink != NULL,
    "ext_read: lookup starting from nested.link should resolve inner.txt.\n");
  assert(node_is_file(from_symlink),
    "ext_read: lookup starting from nested.link should return a regular file.\n");

  char* from_symlink_text = read_node_text(from_symlink);
  assert(streq(from_symlink_text, "Nested hello\n"),
    "ext_read: lookup starting from nested.link returned the wrong contents.\n");
  say("***Symlink dir lookup: ok\n", NULL);

  int target_args[1] = { (int)nested_target };
  say("***Symlink target: %s\n", target_args);

  free(from_symlink_text);
  node_free(from_symlink);
  free(absolute_nested_link_text);
  node_free(absolute_nested_link_path);
  free(dotted_nested_link_text);
  node_free(dotted_nested_link_path);
  free(nested_target);
  node_free(nested_link);
  node_free(absolute_hello);
  free(nested_link_text);
  node_free(nested_link_path);
  free(nested_path_text);
  node_free(nested_file_path);
  free(nested_text);
  node_free(nested_file);
  node_free(nested);
}

// blocks.txt keeps a fixed byte layout, so the historical BLOCK1-MARKER name
// only lines up with logical block 1 when the ext2 block size is 1024 bytes.
// The test therefore finds the marker first, then re-reads whichever logical
// block currently contains it and a small window around that same offset.
static void check_blocks_fixture(struct Node* root, unsigned block_size) {
  struct Node* blocks = node_find(root, "blocks.txt");
  assert(blocks != NULL, "ext_read: blocks.txt not found in root directory.\n");
  assert(node_is_file(blocks), "ext_read: blocks.txt should decode as a regular file.\n");

  unsigned blocks_size = node_size_in_bytes(blocks);
  char* blocks_text = read_node_text(blocks);
  int marker_offset = find_marker(blocks_text, blocks_size, BLOCKS_MARKER);
  assert(marker_offset >= 0,
    "ext_read: blocks.txt should contain BLOCK1-MARKER.\n");

  unsigned marker_offset_u = marker_offset;
  unsigned marker_block = marker_offset_u / block_size;
  unsigned marker_block_offset = marker_offset_u % block_size;

  if (blocks_size > block_size) {
    assert(marker_block > 0,
      "ext_read: blocks.txt should place BLOCK1-MARKER in a later block when the file spans multiple ext2 blocks.\n");
  } else {
    assert(marker_block == 0,
      "ext_read: blocks.txt should keep BLOCK1-MARKER in block 0 when the file fits in one ext2 block.\n");
  }

  char* marker_block_data = malloc(block_size);
  node_read_block(blocks, marker_block, marker_block_data);
  int direct_marker_offset = find_marker(marker_block_data, block_size, BLOCKS_MARKER);
  assert(direct_marker_offset == (int)marker_block_offset,
    "ext_read: node_read_block returned the wrong logical block for BLOCK1-MARKER.\n");

  // This window is cross-block for the 1024-byte image and marker-centered for
  // larger block sizes where the whole fixture fits inside block 0.
  unsigned window_offset = 0;
  unsigned window_size = 0;
  choose_marker_window(marker_offset_u, blocks_size, block_size, &window_offset,
    &window_size);
  char* marker_window = malloc(window_size);
  node_read_all(blocks, window_offset, window_size, marker_window);
  int window_marker = find_marker(marker_window, window_size, BLOCKS_MARKER);
  assert(window_marker == (int)(marker_offset_u - window_offset),
    "ext_read: node_read_all marker window lost the expected marker position.\n");

  int file_end_offset = find_marker(blocks_text, blocks_size, FILE_END_MARKER);
  assert(file_end_offset == (int)(blocks_size - strlen(FILE_END_MARKER)),
    "ext_read: node_read_all did not preserve the expected file tail.\n");

  free(marker_window);
  free(marker_block_data);
  free(blocks_text);
  node_free(blocks);
  say("***blocks.txt reads: ok\n", NULL);
}

// Read helpers should stop at EOF instead of fabricating extra bytes from
// unallocated blocks, and a zero-byte result should leave the destination
// buffer untouched.
static void check_eof_short_reads(struct Node* root) {
  struct Node* hello = node_find(root, "hello.txt");
  char buffer[8];
  unsigned eof_offset;
  unsigned cnt;

  assert(hello != NULL, "ext_read: hello.txt should exist for EOF checks.\n");
  eof_offset = node_size_in_bytes(hello);

  memset(buffer, '?', sizeof(buffer));
  cnt = node_read_all(hello, 3, sizeof(buffer), buffer);
  assert(cnt == 3,
    "ext_read: read across EOF should return only the bytes before EOF.\n");
  assert(strneq(buffer, "lo!", 3),
    "ext_read: EOF-clamped read returned the wrong file tail.\n");
  assert(buffer[3] == '?',
    "ext_read: EOF-clamped read should not overwrite bytes past the short result.\n");

  memset(buffer, '?', sizeof(buffer));
  cnt = node_read_all(hello, eof_offset, sizeof(buffer), buffer);
  assert(cnt == 0,
    "ext_read: a read that starts exactly at EOF should return zero bytes.\n");
  assert(buffer[0] == '?',
    "ext_read: a zero-byte EOF read should not modify the destination buffer.\n");

  node_free(hello);
  say("***EOF short reads: ok\n", NULL);
}

// A missing single-component path should still fail cleanly.
static void check_missing_path(struct Node* root) {
  struct Node* missing = node_find(root, "missing");
  assert(missing == NULL,
    "ext_read: missing should return NULL for a missing root entry.\n");
  say("***Missing path: ok\n", NULL);
}

// Reads the small hello.txt fixture from a fresh lookup. All workers run this
// against the shared filesystem at the same time after the start barrier, so it
// exercises concurrent cache hits on the same hot file path.
static void check_concurrent_hello_once(unsigned block_size) {
  struct Node* hello = node_find(&fs.root, "hello.txt");
  assert(hello != NULL, "ext_read: concurrent hello.txt lookup failed.\n");
  assert(node_is_file(hello),
    "ext_read: concurrent hello.txt lookup did not return a regular file.\n");

  char* hello_block = malloc(block_size + 1);
  node_read_block(hello, 0, hello_block);
  hello_block[node_size_in_bytes(hello)] = 0;
  assert(streq(hello_block, "Hello!"),
    "ext_read: concurrent hello.txt read returned the wrong contents.\n");

  free(hello_block);
  node_free(hello);
}

// Reads a small marker window from blocks.txt. For the 1024-byte image this
// crosses the block boundary before BLOCK1-MARKER; for larger block sizes it
// stays inside block 0. Running this in many threads at once still stresses the
// shared block cache while proving each worker receives an intact marker slice.
static void check_concurrent_blocks_once(unsigned block_size) {
  struct Node* blocks = node_find(&fs.root, "blocks.txt");
  assert(blocks != NULL, "ext_read: concurrent blocks.txt lookup failed.\n");
  assert(node_is_file(blocks),
    "ext_read: concurrent blocks.txt lookup did not return a regular file.\n");

  unsigned blocks_size = node_size_in_bytes(blocks);
  char* blocks_text = read_node_text(blocks);
  int marker_offset = find_marker(blocks_text, blocks_size, BLOCKS_MARKER);
  assert(marker_offset >= 0,
    "ext_read: concurrent blocks.txt read should contain BLOCK1-MARKER.\n");
  unsigned marker_offset_u = marker_offset;

  unsigned window_offset = 0;
  unsigned window_size = 0;
  choose_marker_window(marker_offset_u, blocks_size, block_size, &window_offset,
    &window_size);
  char* marker_window = malloc(window_size);
  node_read_all(blocks, window_offset, window_size, marker_window);
  assert(find_marker(marker_window, window_size, BLOCKS_MARKER) >= 0,
    "ext_read: concurrent marker window read lost BLOCK1-MARKER.\n");

  free(blocks_text);
  free(marker_window);
  node_free(blocks);
}

// Each worker runs the same sequence after a shared start barrier. The barrier
// forces all readers to enter the hottest inode-cache and block-cache paths
// together
static void concurrent_reader_thread(void* arg) {
  struct ConcurrentReadArgs* read_args = (struct ConcurrentReadArgs*)arg;

  barrier_sync(&concurrent_start_barrier);

  for (unsigned round = 0; round < CONCURRENT_ROUNDS; ++round) {
    check_concurrent_hello_once(read_args->block_size);
    __atomic_fetch_add(&concurrent_hello_reads, 1);

    check_concurrent_blocks_once(read_args->block_size);
    __atomic_fetch_add(&concurrent_block_reads, 1);
  }

  __atomic_fetch_add(&concurrent_finished, 1);
}

// Spawns several readers that all start together. The worker mix focuses the
// concurrent phase on inode-cache hits/misses plus block-cache reads; the
// sequential phase above already validates directory, symlink, and missing-path
// semantics in detail.
static void check_concurrent_reads(unsigned block_size) {
  int expected = CONCURRENT_READERS * CONCURRENT_ROUNDS;

  concurrent_finished = 0;
  concurrent_hello_reads = 0;
  concurrent_block_reads = 0;
  int args[3] = { CONCURRENT_READERS, CONCURRENT_ROUNDS, expected };

  say("***Concurrent reads start: threads=%d rounds=%d ops=%d\n", args);

  barrier_init(&concurrent_start_barrier, CONCURRENT_READERS + 1);

  for (unsigned i = 0; i < CONCURRENT_READERS; ++i) {
    struct ConcurrentReadArgs* args = malloc(sizeof(struct ConcurrentReadArgs));
    assert(args != NULL, "ext_read: concurrent reader args allocation failed.\n");
    args->block_size = block_size;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "ext_read: concurrent reader Fun allocation failed.\n");
    fun->func = concurrent_reader_thread;
    fun->arg = args;

    thread(fun);
  }

  // The main test thread enters the same barrier as the workers. It blocks
  // here until every worker is ready, then all of them start the read phase
  // together.
  barrier_sync(&concurrent_start_barrier);

  while (__atomic_load_n(&concurrent_finished) != CONCURRENT_READERS) {
    yield();
  }

  barrier_destroy(&concurrent_start_barrier);

  assert(__atomic_load_n(&concurrent_hello_reads) == expected,
    "ext_read: concurrent hello.txt read count did not match the thread plan.\n");
  assert(__atomic_load_n(&concurrent_block_reads) == expected,
    "ext_read: concurrent blocks.txt read count did not match the thread plan.\n");

  say("***Concurrent reads: threads=%d rounds=%d ops=%d\n", args);
}

// Run the sequential ext2 read checks first, then the short concurrent phase.
int kernel_main(void) {
  // The ext2 image is built from tests/ext_read.dir. Each helper targets one
  // part of the ext2 implementation so the test can identify which behavior
  // regressed. The test starts with single-threaded validation, then launches
  // concurrent readers to stress the same code paths under contention.
  say("***Hello from ext2 test!\n", NULL);

  int block_size = ext2_get_block_size(&fs);
  int inode_size = ext2_get_inode_size(&fs);
  assert(block_size == EXT2_BLOCK_SIZE_1K || block_size == EXT2_BLOCK_SIZE_2K ||
    block_size == EXT2_BLOCK_SIZE_4K,
    "ext_read: ext2 block size should match the supported mkfs.ext2 test sizes.\n");
  assert(inode_size >= sizeof(struct Inode),
    "ext_read: superblock inode size is smaller than the in-memory inode decoder.\n");
  say("***Filesystem geometry: ok\n", NULL);

  struct Node* root = &fs.root;
  assert(node_is_dir(root), "ext_read: root inode should be a directory.\n");
  assert(node_get_type(root) == EXT2_S_IFDIR,
    "ext_read: root inode type did not decode as a directory.\n");
  assert(node_entry_count(root) >= 5,
    "ext_read: root directory should contain '.', '..', and the test fixtures.\n");

  // Cover path lookup and content reads before stressing cache sharing.
  check_root_lookup_contract(root);
  unsigned hello_inumber = check_hello_file(root, block_size);
  check_nested_entries(root, hello_inumber);
  check_blocks_fixture(root, block_size);
  check_eof_short_reads(root);
  check_missing_path(root);
  // Finish with the concurrent cache-read phase.
  check_concurrent_reads(block_size);

  return 0;
}
