#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/interrupts.h"

struct Ext2 fs;

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

// Exercises directory metadata, lookup from a non-root directory, absolute-path
// reset back to the filesystem root, and symlink target decoding.
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

  int target_args[1] = { (int)nested_target };
  say("***Symlink target: %s\n", target_args);

  free(nested_target);
  node_free(nested_link);
  node_free(absolute_hello);
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

int kernel_main(void) {
  // The ext2 image is built from tests/ext_read.dir. Each helper targets one
  // part of the ext2 implementation so the test can identify which behavior
  // regressed: direct file reads, directory metadata, absolute-path handling,
  // symlink decoding, and multi-block file reads.
  say("***Hello from ext2 test!\n", NULL);

  unsigned saved_imr = disable_interrupts();

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

  unsigned hello_inumber = check_hello_file(root, block_size);
  check_nested_entries(root, hello_inumber);
  check_multi_block_file(root, block_size);
  check_missing_path(root);

  ext2_destroy(&fs);
  restore_interrupts(saved_imr);

  return 0;
}
