/*
 * Verifies ext2 delete removes regular files, symlinks, and empty directories,
 * and that those deletes restore inode counts, block counts, and parent link
 * counts. The test also forces a delete-then-recreate path so reused blocks do
 * not leak stale cached bytes into the new file's zero-filled gap.
 */

#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

struct Ext2 fs;

#define DELETE_TEST_SHORT_TARGET "tiny-target"
#define DELETE_TEST_LONG_TARGET_LEN 80
#define DELETE_TEST_LONG_TARGET_BYTES 81
#define DELETE_TEST_SINGLE_INDIRECT_DATA_BLOCKS 13
#define DELETE_TEST_SINGLE_INDIRECT_TOTAL_BLOCKS 14
#define DELETE_TEST_REUSE_GAP_OFFSET 4
#define DELETE_TEST_REUSE_PAYLOAD "NEW"
#define DELETE_TEST_REUSE_WINDOW_BYTES 7

static char* alloc_filled_bytes(unsigned size, char byte, char* failure_message) {
  char* buf = malloc(size);
  assert(size == 0 || buf != NULL, failure_message);
  memset(buf, byte, size);
  return buf;
}

static void check_fixture_delete(struct Node* root) {
  struct Node* file_to_delete = node_find(root, "delete_me.txt");
  struct Node* dir_to_delete = node_find(root, "dir");
  struct Node* orphan;
  struct Node* deleted;
  struct Node* deleted_dir;

  assert(file_to_delete != NULL, "node_find: failed to find the file to delete.\n");
  node_free(file_to_delete);

  assert(dir_to_delete != NULL, "node_find: failed to find the directory to delete.\n");
  assert(node_entry_count(dir_to_delete) == 3,
    "ext_delete: dir should contain '.', '..', and orphan.txt before deleting it.\n");

  orphan = node_find(dir_to_delete, "orphan.txt");
  assert(orphan != NULL, "node_find: failed to find the nested file to delete.\n");
  node_free(orphan);

  node_delete(root, "delete_me.txt");
  node_delete(dir_to_delete, "orphan.txt");

  orphan = node_find(dir_to_delete, "orphan.txt");
  assert(orphan == NULL, "node_find: found the nested file that was supposed to be deleted.\n");
  assert(node_entry_count(dir_to_delete) == 2,
    "ext_delete: dir should be empty except for '.' and '..' before deleting it.\n");
  node_free(dir_to_delete);

  node_delete(root, "dir");

  deleted = node_find(root, "delete_me.txt");
  assert(deleted == NULL, "node_find: found the file that was supposed to be deleted.\n");

  deleted_dir = node_find(root, "dir");
  assert(deleted_dir == NULL, "node_find: found the directory that was supposed to be deleted.\n");
}

static void fill_long_target(char* dest) {
  unsigned i;

  for (i = 0; i < DELETE_TEST_LONG_TARGET_LEN; ++i){
    dest[i] = 'a' + (i % 26);
  }
  dest[DELETE_TEST_LONG_TARGET_LEN] = 0;
}

static void check_dynamic_delete_coverage(struct Node* root) {
  unsigned baseline_blocks = root->filesystem->superblock.free_blocks_count;
  unsigned baseline_inodes = root->filesystem->superblock.free_inodes_count;
  unsigned baseline_root_links = node_get_num_links(root);
  unsigned expected_blocks = baseline_blocks;
  unsigned expected_inodes = baseline_inodes;
  unsigned block_size;
  unsigned indirect_bytes;
  unsigned cnt;
  unsigned freed_block;
  unsigned i;
  char long_target[DELETE_TEST_LONG_TARGET_BYTES];
  char reuse_window[DELETE_TEST_REUSE_WINDOW_BYTES];
  char* indirect_payload;
  char* reuse_src_buf;
  struct Node* scratch;
  struct Node* short_link;
  struct Node* long_link;
  struct Node* indirect;
  struct Node* reuse_src;
  struct Node* reuse_dst;

  scratch = node_make_dir(root, "delete-scratch");
  assert(scratch != NULL, "ext_delete: failed to create the scratch directory.\n");
  expected_blocks -= 1; // new directory gets one data block for ".", "..", and future entries
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating the scratch directory should consume exactly one block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the scratch directory should consume exactly one inode.\n");
  assert(node_get_num_links(root) == baseline_root_links + 1,
    "ext_delete: creating a child directory should increment the parent link count.\n");

  short_link = node_make_symlink(scratch, "short-link", DELETE_TEST_SHORT_TARGET);
  assert(short_link != NULL, "ext_delete: failed to create the short symlink.\n");
  node_free(short_link);
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating a fast symlink should not consume a data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the short symlink should consume exactly one inode.\n");

  node_delete(scratch, "short-link");
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting a fast symlink should not change the free block count.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the short symlink should restore the free inode count.\n");

  fill_long_target(long_target);

  long_link = node_make_symlink(scratch, "long-link", long_target);
  assert(long_link != NULL, "ext_delete: failed to create the long symlink.\n");
  node_free(long_link);
  expected_blocks -= 1; // long target fits in one ext2 data block
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating the long symlink should consume one data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the long symlink should consume one inode.\n");

  node_delete(scratch, "long-link");
  expected_blocks += 1;
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting the long symlink should restore its data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the long symlink should restore its inode.\n");

  block_size = ext2_get_block_size(root->filesystem);
  indirect_bytes = block_size * DELETE_TEST_SINGLE_INDIRECT_DATA_BLOCKS;
  indirect_payload = alloc_filled_bytes(indirect_bytes, 'Q',
    "ext_delete: failed to allocate the large-file payload.\n");
  indirect = node_make_file(scratch, "indirect.bin");
  assert(indirect != NULL, "ext_delete: failed to create the single-indirect test file.\n");
  cnt = node_write_all(indirect, 0, indirect_bytes, indirect_payload);
  assert(cnt == indirect_bytes, "ext_delete: failed to write the large file payload.\n");
  node_free(indirect);
  free(indirect_payload);
  expected_blocks -= DELETE_TEST_SINGLE_INDIRECT_TOTAL_BLOCKS;
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating the single-indirect test file should consume its data and metadata blocks.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the single-indirect test file should consume one inode.\n");

  node_delete(scratch, "indirect.bin");
  expected_blocks += DELETE_TEST_SINGLE_INDIRECT_TOTAL_BLOCKS;
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting the single-indirect test file should restore every allocated block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the single-indirect test file should restore its inode.\n");

  reuse_src_buf = alloc_filled_bytes(block_size, 'R',
    "ext_delete: failed to allocate the reuse-source payload.\n");
  reuse_src = node_make_file(scratch, "reuse-src.bin");
  assert(reuse_src != NULL, "ext_delete: failed to create the reuse-source file.\n");
  cnt = node_write_all(reuse_src, 0, block_size, reuse_src_buf);
  assert(cnt == block_size, "ext_delete: failed to write the reuse-source file.\n");
  freed_block = reuse_src->cached->inode.block[0];
  node_free(reuse_src);
  free(reuse_src_buf);
  expected_blocks -= 1;
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating the reuse-source file should consume one data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the reuse-source file should consume one inode.\n");

  node_delete(scratch, "reuse-src.bin");
  expected_blocks += 1;
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting the reuse-source file should restore its data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the reuse-source file should restore its inode.\n");

  reuse_dst = node_make_file(scratch, "reuse-dst.bin");
  assert(reuse_dst != NULL, "ext_delete: failed to create the reuse-destination file.\n");
  cnt = node_write_all(reuse_dst, DELETE_TEST_REUSE_GAP_OFFSET, strlen(DELETE_TEST_REUSE_PAYLOAD),
    DELETE_TEST_REUSE_PAYLOAD);
  assert(cnt == strlen(DELETE_TEST_REUSE_PAYLOAD),
    "ext_delete: failed to write the reuse-destination payload.\n");
  assert(reuse_dst->cached->inode.block[0] == freed_block,
    "ext_delete: expected this delete/recreate path to reuse the freed data block.\n");

  cnt = node_read_all(reuse_dst, 0, DELETE_TEST_REUSE_GAP_OFFSET + strlen(DELETE_TEST_REUSE_PAYLOAD), reuse_window);
  assert(cnt == DELETE_TEST_REUSE_GAP_OFFSET + strlen(DELETE_TEST_REUSE_PAYLOAD),
    "ext_delete: failed to read back the reuse-destination file.\n");
  for (i = 0; i < DELETE_TEST_REUSE_GAP_OFFSET; ++i){
    assert(reuse_window[i] == 0,
      "ext_delete: reused blocks must not leak stale bytes into a zero-filled gap.\n");
  }
  for (i = 0; i < strlen(DELETE_TEST_REUSE_PAYLOAD); ++i){
    assert(reuse_window[DELETE_TEST_REUSE_GAP_OFFSET + i] == DELETE_TEST_REUSE_PAYLOAD[i],
      "ext_delete: reused blocks did not preserve the newly written payload.\n");
  }
  node_free(reuse_dst);
  expected_blocks -= 1;
  expected_inodes -= 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: creating the reuse-destination file should consume one data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: creating the reuse-destination file should consume one inode.\n");

  node_delete(scratch, "reuse-dst.bin");
  expected_blocks += 1;
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting the reuse-destination file should restore its data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the reuse-destination file should restore its inode.\n");

  assert(node_entry_count(scratch) == 2,
    "ext_delete: scratch directory should be empty except for '.' and '..' before deletion.\n");
  node_free(scratch);

  node_delete(root, "delete-scratch");
  expected_blocks += 1;
  expected_inodes += 1;
  assert(root->filesystem->superblock.free_blocks_count == expected_blocks,
    "ext_delete: deleting the scratch directory should restore its directory block.\n");
  assert(root->filesystem->superblock.free_inodes_count == expected_inodes,
    "ext_delete: deleting the scratch directory should restore its inode.\n");
  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext_delete: all dynamic delete coverage should restore the starting free block count.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext_delete: all dynamic delete coverage should restore the starting free inode count.\n");
  assert(node_get_num_links(root) == baseline_root_links,
    "ext_delete: deleting the scratch directory should restore the parent link count.\n");
}

int kernel_main(void) {
  say("***Hello from ext2 delete test!\n", NULL);

  ext2_init(&fs);
  check_fixture_delete(&fs.root);
  check_dynamic_delete_coverage(&fs.root);

  ext2_destroy(&fs);

  return 0;
}
