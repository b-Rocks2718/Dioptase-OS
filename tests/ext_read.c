#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

struct Ext2 fs;

#define CONCURRENT_READERS 4
#define CONCURRENT_ROUNDS 1

struct ConcurrentReadArgs {
  unsigned block_size;
};

static struct Barrier concurrent_start_barrier;
static int concurrent_finished = 0;
static int concurrent_hello_reads = 0;
static int concurrent_block_reads = 0;
static int concurrent_directory_checks = 0;
static int concurrent_symlink_checks = 0;
static int concurrent_missing_checks = 0;

// Verifies the ownership contract for root lookups: ext2_find("/") should
// return a heap-owned wrapper that callers can release with node_free().
static void check_root_lookup_contract(struct Node* root) {
  struct Node* looked_up_root = ext2_find(&fs, root, "/");
  assert(looked_up_root != NULL,
    "ext_read: root lookup should return a valid node.\n");
  assert(node_is_dir(looked_up_root),
    "ext_read: root lookup should decode as a directory.\n");
  assert(looked_up_root->inumber == root->inumber,
    "ext_read: root lookup should resolve to the root inode number.\n");
  assert(looked_up_root != root,
    "ext_read: ext2_find should return a heap wrapper for root lookups.\n");
  assert(looked_up_root->inode != root->inode,
    "ext_read: root lookup should return an owned inode copy.\n");

  node_free(looked_up_root);
  say("***Root lookup ownership: ok\n", NULL);
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
  struct Node* hello = ext2_find(&fs, root, "hello.txt");
  assert(hello != NULL, "ext_read: hello.txt not found in root directory.\n");
  assert(node_is_file(hello), "ext_read: hello.txt should decode as a regular file.\n");
  assert(node_get_type(hello) == EXT2_S_IFREG,
    "ext_read: hello.txt inode type did not decode as a regular file.\n");

  char* hello_block = malloc(block_size + 1);
  node_read_block(hello, 0, hello_block);
  hello_block[node_size_in_bytes(hello)] = 0;
  assert(streq(hello_block, "Hello!"),
    "ext_read: node_read_block did not return the expected hello.txt contents.\n");

  int hello_args[1] = { (int)hello_block };
  say("***Hello content: %s\n", hello_args);

  unsigned hello_inumber = hello->inumber;

  free(hello_block);
  node_free(hello);

  return hello_inumber;
}

// Exercises directory metadata, multi-component path traversal, lookup from a
// non-root directory, absolute-path reset back to the filesystem root, and
// symlink target decoding.
static void check_nested_entries(struct Node* root, unsigned hello_inumber) {
  struct Node* nested = ext2_find(&fs, root, "nested");
  assert(nested != NULL, "ext_read: nested directory not found in root directory.\n");
  assert(node_is_dir(nested), "ext_read: nested should decode as a directory.\n");

  unsigned nested_entries = node_entry_count(nested);
  assert(nested_entries == 3,
    "ext_read: nested should contain '.', '..', and inner.txt only.\n");
  assert(node_get_num_links(nested) == 2,
    "ext_read: nested should have exactly two directory links.\n");
  say("***Nested metadata: ok\n", NULL);

  struct Node* nested_file = ext2_find(&fs, nested, "inner.txt");
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
  struct Node* nested_file_path = ext2_find(&fs, root, "nested/inner.txt");
  assert(nested_file_path != NULL,
    "ext_read: nested/inner.txt multi-component lookup failed.\n");
  assert(node_is_file(nested_file_path),
    "ext_read: nested/inner.txt path lookup should return a regular file.\n");

  char* nested_path_text = read_node_text(nested_file_path);
  assert(streq(nested_path_text, "Nested hello\n"),
    "ext_read: multi-component nested/inner.txt returned the wrong contents.\n");

  struct Node* nested_link_path = ext2_find(&fs, root, "nested.link/inner.txt");
  assert(nested_link_path != NULL,
    "ext_read: nested.link/inner.txt symlink traversal lookup failed.\n");
  assert(node_is_file(nested_link_path),
    "ext_read: nested.link/inner.txt should resolve to a regular file.\n");

  char* nested_link_text = read_node_text(nested_link_path);
  assert(streq(nested_link_text, "Nested hello\n"),
    "ext_read: symlink-expanded multi-component lookup returned the wrong contents.\n");
  say("***Multi-part paths: ok\n", NULL);

  struct Node* absolute_hello = ext2_find(&fs, nested, "/hello.txt");
  assert(absolute_hello != NULL,
    "ext_read: absolute path lookup should restart from the ext2 root.\n");
  assert(absolute_hello->inumber == hello_inumber,
    "ext_read: /hello.txt should resolve to the same inode as hello.txt.\n");

  struct Node* nested_link = ext2_find(&fs, root, "nested.link");
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
  struct Node* from_symlink = ext2_find(&fs, nested_link, "inner.txt");
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

// blocks.txt is deliberately larger than one 1KiB ext2 block in the default
// test image. Reading the whole file and then reading block 1 directly checks
// both the sequential read path and direct block addressing.
static void check_multi_block_file(struct Node* root, unsigned block_size) {
  struct Node* blocks = ext2_find(&fs, root, "blocks.txt");
  assert(blocks != NULL, "ext_read: blocks.txt not found in root directory.\n");
  assert(node_is_file(blocks), "ext_read: blocks.txt should decode as a regular file.\n");
  assert(node_size_in_bytes(blocks) > block_size,
    "ext_read: blocks.txt should span multiple ext2 blocks.\n");

  unsigned blocks_size = node_size_in_bytes(blocks);
  char* blocks_text = read_node_text(blocks);
  int marker_offset = find_marker(blocks_text, blocks_size, "BLOCK1-MARKER\n");
  assert(marker_offset >= (int)block_size,
    "ext_read: BLOCK1-MARKER should appear after the first ext2 block.\n");

  char* block_one = malloc(block_size);
  node_read_block(blocks, 1, block_one);
  int block_one_offset = find_marker(block_one, block_size, "BLOCK1-MARKER\n");
  assert(block_one_offset >= 0,
    "ext_read: node_read_block(blocks.txt, 1) did not include BLOCK1-MARKER.\n");

  int block_one_args[1] = { block_one_offset };
  say("***Block1 marker offset: %d\n", block_one_args);

  // This unaligned read starts near the end of block 0 and extends into block
  // 1. It catches bugs where `node_read_all` copies the second chunk to the
  // wrong destination offset when the first chunk is only a partial block.
  unsigned window_offset = block_size - 32;
  unsigned window_size = 320;
  char* marker_window = malloc(window_size);
  node_read_all(blocks, window_offset, window_size, marker_window);
  int window_marker = find_marker(marker_window, window_size, "BLOCK1-MARKER\n");
  assert(window_marker == marker_offset - (int)window_offset,
    "ext_read: unaligned node_read_all lost the expected marker position.\n");

  int file_end_offset = find_marker(blocks_text, blocks_size, "FILE-END\n");
  assert(file_end_offset == (int)(blocks_size - strlen("FILE-END\n")),
    "ext_read: node_read_all did not preserve the expected file tail.\n");

  free(marker_window);
  free(block_one);
  free(blocks_text);
  node_free(blocks);
}

// A missing single-component path should still fail cleanly.
static void check_missing_path(struct Node* root) {
  struct Node* missing = ext2_find(&fs, root, "missing");
  assert(missing == NULL,
    "ext_read: missing should return NULL for a missing root entry.\n");
  say("***Missing path: ok\n", NULL);
}

// Reads the small hello.txt fixture from a fresh lookup. All workers run this
// against the shared filesystem at the same time after the start barrier, so it
// exercises concurrent cache hits on the same hot file path.
static void check_concurrent_hello_once(unsigned block_size) {
  struct Node* hello = ext2_find(&fs, &fs.root, "hello.txt");
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

// Reads across the block boundary in blocks.txt. Running this in many threads at
// once stresses the shared block cache while also proving each worker receives
// an intact cross-block slice instead of mixed data from another read.
static void check_concurrent_blocks_once(unsigned block_size) {
  struct Node* blocks = ext2_find(&fs, &fs.root, "blocks.txt");
  assert(blocks != NULL, "ext_read: concurrent blocks.txt lookup failed.\n");
  assert(node_is_file(blocks),
    "ext_read: concurrent blocks.txt lookup did not return a regular file.\n");

  unsigned window_offset = block_size - 32;
  unsigned window_size = 320;
  char* marker_window = malloc(window_size);
  node_read_all(blocks, window_offset, window_size, marker_window);
  assert(find_marker(marker_window, window_size, "BLOCK1-MARKER\n") >= 0,
    "ext_read: concurrent cross-block read lost BLOCK1-MARKER.\n");

  free(marker_window);
  node_free(blocks);
}

// Uses only single-component ext2_find calls so every intermediate node can be
// freed explicitly. This still exercises shared directory traversal by having
// each thread scan the root and nested directories at the same time.
static void check_concurrent_directory_once(void) {
  assert(node_entry_count(&fs.root) >= 5,
    "ext_read: concurrent root directory count regressed.\n");

  struct Node* nested = ext2_find(&fs, &fs.root, "nested");
  assert(nested != NULL, "ext_read: concurrent nested lookup failed.\n");
  assert(node_is_dir(nested),
    "ext_read: concurrent nested lookup did not return a directory.\n");
  assert(node_entry_count(nested) == 3,
    "ext_read: concurrent nested directory count regressed.\n");

  struct Node* inner = ext2_find(&fs, nested, "inner.txt");
  assert(inner != NULL, "ext_read: concurrent inner.txt lookup failed.\n");
  assert(node_is_file(inner),
    "ext_read: concurrent inner.txt lookup did not return a regular file.\n");

  char* inner_text = read_node_text(inner);
  assert(streq(inner_text, "Nested hello\n"),
    "ext_read: concurrent nested file read returned the wrong contents.\n");

  free(inner_text);
  node_free(inner);
  node_free(nested);
}

// Mixes a symlink target decode with a failing lookup. That covers two common
// "control path" operations under contention: short symlink reads and clean
// NULL returns when the requested entry does not exist.
static void check_concurrent_symlink_and_missing_once(void) {
  struct Node* nested_link = ext2_find(&fs, &fs.root, "nested.link");
  assert(nested_link != NULL, "ext_read: concurrent nested.link lookup failed.\n");
  assert(node_is_symlink(nested_link),
    "ext_read: concurrent nested.link lookup did not return a symlink.\n");

  char* target = malloc(node_size_in_bytes(nested_link) + 1);
  node_get_symlink_target(nested_link, target);
  assert(streq(target, "nested"),
    "ext_read: concurrent nested.link read returned the wrong target.\n");

  struct Node* missing = ext2_find(&fs, &fs.root, "missing");
  assert(missing == NULL,
    "ext_read: concurrent missing lookup should still return NULL.\n");

  free(target);
  node_free(nested_link);
}

// Each worker runs the same sequence after a shared start barrier. The barrier
// forces all readers to enter the same ext2 paths together, which is enough to
// exercise real contention on the shared caches and directory traversal logic
// without adding extra scheduler churn inside the worker itself.
static void concurrent_reader_thread(void* arg) {
  struct ConcurrentReadArgs* read_args = (struct ConcurrentReadArgs*)arg;

  barrier_sync(&concurrent_start_barrier);

  for (unsigned round = 0; round < CONCURRENT_ROUNDS; ++round) {
    check_concurrent_hello_once(read_args->block_size);
    __atomic_fetch_add(&concurrent_hello_reads, 1);

    check_concurrent_blocks_once(read_args->block_size);
    __atomic_fetch_add(&concurrent_block_reads, 1);

    check_concurrent_directory_once();
    __atomic_fetch_add(&concurrent_directory_checks, 1);

    check_concurrent_symlink_and_missing_once();
    __atomic_fetch_add(&concurrent_symlink_checks, 1);
    __atomic_fetch_add(&concurrent_missing_checks, 1);
  }

  __atomic_fetch_add(&concurrent_finished, 1);
}

// Spawns several readers that all start together. The worker mix covers
// concurrent inode-cache hits/misses, block-cache reads, directory scans,
// symlink target decoding, and missing-path lookups.
static void check_concurrent_reads(unsigned block_size) {
  int expected = CONCURRENT_READERS * CONCURRENT_ROUNDS;

  concurrent_finished = 0;
  concurrent_hello_reads = 0;
  concurrent_block_reads = 0;
  concurrent_directory_checks = 0;
  concurrent_symlink_checks = 0;
  concurrent_missing_checks = 0;

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
  assert(__atomic_load_n(&concurrent_directory_checks) == expected,
    "ext_read: concurrent directory check count did not match the thread plan.\n");
  assert(__atomic_load_n(&concurrent_symlink_checks) == expected,
    "ext_read: concurrent symlink check count did not match the thread plan.\n");
  assert(__atomic_load_n(&concurrent_missing_checks) == expected,
    "ext_read: concurrent missing-path count did not match the thread plan.\n");

  int args[3] = { CONCURRENT_READERS, CONCURRENT_ROUNDS, expected };
  say("***Concurrent reads: threads=%d rounds=%d ops=%d\n", args);
}

int kernel_main(void) {
  // The ext2 image is built from tests/ext_read.dir. Each helper targets one
  // part of the ext2 implementation so the test can identify which behavior
  // regressed. The test starts with single-threaded validation, then launches
  // concurrent readers to stress the same code paths under contention.
  say("***Hello from ext2 test!\n", NULL);

  ext2_init(&fs);

  int block_size = ext2_get_block_size(&fs);
  int inode_size = ext2_get_inode_size(&fs);
  say("***Block size: %d\n", &block_size);
  say("***Inode size: %d\n", &inode_size);

  struct Node* root = &fs.root;
  assert(node_is_dir(root), "ext_read: root inode should be a directory.\n");
  assert(node_get_type(root) == EXT2_S_IFDIR,
    "ext_read: root inode type did not decode as a directory.\n");
  assert(node_entry_count(root) >= 5,
    "ext_read: root directory should contain '.', '..', and the test fixtures.\n");

  check_root_lookup_contract(root);
  unsigned hello_inumber = check_hello_file(root, block_size);
  check_nested_entries(root, hello_inumber);
  check_multi_block_file(root, block_size);
  check_missing_path(root);
  check_concurrent_reads(block_size);

  ext2_destroy(&fs);
  return 0;
}
