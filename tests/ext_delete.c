/*
 * ext2 delete test.
 *
 * Validates:
 * - deleting regular files, symlinks, and empty directories removes the names
 *   and restores inode, block, and link-count accounting
 * - fast symlinks and block-backed symlinks free the right resources
 * - deleting and recreating files does not leak stale cached data into reused
 *   blocks or zero-filled gaps
 * - final unlink does not reclaim inode/block resources until the last live
 *   wrapper releases the target inode
 * - an already-open directory wrapper cannot recreate namespace state after the
 *   directory has been unlinked
 *
 * How:
 * - hold an extra wrapper on one file while another thread unlinks it, then
 *   verify pathname removal happens immediately while block/inode reclamation
 *   waits until that wrapper is released
 * - hold an extra wrapper on one empty directory while another thread unlinks
 *   it, then verify creates through the stale wrapper are rejected and final
 *   reclamation waits for the wrapper release
 * - delete the fixed fixture entries from tests/ext_delete.dir and confirm they
 *   disappear from lookup results
 * - build a scratch directory and walk through short symlink, long symlink,
 *   single-indirect file, and delete-then-recreate coverage while checking the
 *   free block and free inode counters after each step
 * - delete the scratch directory and confirm all counts return to baseline
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
#define DELETE_TEST_BUSY_FILE_NAME "busy-file.bin"
#define DELETE_TEST_BUSY_FILE_ROUNDS 4
#define DELETE_TEST_BUSY_DIR_NAME "busy-dir"
#define DELETE_TEST_BUSY_DIR_CHILD_NAME "late.txt"

struct ConcurrentDeleteFileArgs {
  struct Barrier* start;
  int* ready_to_release;
  int* release;
  int* done;
  unsigned block_size;
};

struct ConcurrentDeleteDirArgs {
  struct Barrier* start;
  int* delete_done;
  int* ready_to_release;
  int* release;
  int* done;
};

// Allocate a byte buffer pre-filled with one repeated value.
static char* alloc_filled_bytes(unsigned size, char byte, char* failure_message) {
  char* buf = malloc(size);
  assert(size == 0 || buf != NULL, failure_message);
  memset(buf, byte, size);
  return buf;
}

// Keep one unlinked file inode alive through an extra wrapper while the main
// thread deletes the pathname. Writes stay within the pre-sized first block so
// the expected free-block count remains stable until the final release.
static void concurrent_delete_file_worker(void* arg) {
  struct ConcurrentDeleteFileArgs* args = (struct ConcurrentDeleteFileArgs*)arg;
  struct Node* file = node_find(&fs.root, DELETE_TEST_BUSY_FILE_NAME);
  char* payload;
  char* verify;

  assert(file != NULL,
    "ext_delete: concurrent delete worker failed to reopen the busy file.\n");

  payload = alloc_filled_bytes(args->block_size, 'W',
    "ext_delete: failed to allocate the busy-file worker payload.\n");
  verify = malloc(args->block_size);
  assert(verify != NULL,
    "ext_delete: failed to allocate the busy-file readback buffer.\n");

  barrier_sync(args->start);

  for (unsigned round = 0; round < DELETE_TEST_BUSY_FILE_ROUNDS; ++round){
    unsigned cnt = node_write_all(file, 0, args->block_size, payload);
    assert(cnt == args->block_size,
      "ext_delete: busy-file worker write returned the wrong byte count.\n");
    cnt = node_read_all(file, 0, args->block_size, verify);
    assert(cnt == args->block_size,
      "ext_delete: busy-file worker failed to read back the unlinked file.\n");
    for (unsigned i = 0; i < args->block_size; ++i){
      assert(verify[i] == 'W',
        "ext_delete: writes through an unlinked live wrapper lost file data.\n");
    }
    yield();
  }

  free(payload);
  free(verify);
  __atomic_fetch_add(args->ready_to_release, 1);

  while (__atomic_load_n(args->release) == 0) {
    yield();
  }

  node_free(file);
  __atomic_fetch_add(args->done, 1);
}

// The worker keeps an empty directory wrapper alive after the pathname is
// deleted, then proves that the stale wrapper cannot create a new child entry.
static void concurrent_delete_dir_worker(void* arg) {
  struct ConcurrentDeleteDirArgs* args = (struct ConcurrentDeleteDirArgs*)arg;
  struct Node* dir = node_find(&fs.root, DELETE_TEST_BUSY_DIR_NAME);
  struct Node* child;

  assert(dir != NULL,
    "ext_delete: concurrent delete worker failed to reopen the busy directory.\n");

  barrier_sync(args->start);

  while (__atomic_load_n(args->delete_done) == 0) {
    yield();
  }

  child = node_make_file(dir, DELETE_TEST_BUSY_DIR_CHILD_NAME);
  assert(child == NULL,
    "ext_delete: create through an unlinked directory wrapper should fail.\n");
  assert(node_entry_count(dir) == 2,
    "ext_delete: stale directory wrapper should remain empty after unlink.\n");

  __atomic_fetch_add(args->ready_to_release, 1);

  while (__atomic_load_n(args->release) == 0) {
    yield();
  }

  node_free(dir);
  __atomic_fetch_add(args->done, 1);
}

// Delete the fixed fixture entries and confirm they disappear from lookup.
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

// Fill one long symlink target with deterministic text.
static void fill_long_target(char* dest) {
  unsigned i;

  for (i = 0; i < DELETE_TEST_LONG_TARGET_LEN; ++i){
    dest[i] = 'a' + (i % 26);
  }
  dest[DELETE_TEST_LONG_TARGET_LEN] = 0;
}

// Create and delete dynamic entries while checking ext2 accounting after each step.
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

  // Start from a fresh scratch directory so every later delete has one parent.
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

  // First cover fast symlink allocation and deletion.
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

  // Then cover a block-backed symlink target.
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
  // Grow a file into the single-indirect range, then delete it.
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
  // Finally cover delete-then-recreate block reuse and zero-fill behavior.
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

// Covers last-link deletion while another live wrapper still references the
// inode. The pathname must disappear immediately, but ext2 should defer final
// inode/block reclamation until that wrapper releases the cached inode.
static void check_concurrent_delete_file_coverage(struct Node* root) {
  unsigned baseline_blocks = root->filesystem->superblock.free_blocks_count;
  unsigned baseline_inodes = root->filesystem->superblock.free_inodes_count;
  unsigned block_size = ext2_get_block_size(root->filesystem);
  unsigned original_inumber;
  int ready_to_release = 0;
  int release = 0;
  int done = 0;
  struct Barrier start;
  struct Node* file = node_make_file(root, DELETE_TEST_BUSY_FILE_NAME);
  struct Node* deleted;
  char* initial;

  assert(file != NULL, "ext_delete: failed to create the busy delete file.\n");
  original_inumber = file->cached->inumber;
  initial = alloc_filled_bytes(block_size, 'I',
    "ext_delete: failed to allocate the busy-file bootstrap payload.\n");
  unsigned cnt = node_write_all(file, 0, block_size, initial);
  assert(cnt == block_size, "ext_delete: failed to pre-size the busy delete file.\n");
  free(initial);
  node_free(file);

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks - 1,
    "ext_delete: busy-file setup should consume one data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes - 1,
    "ext_delete: busy-file setup should consume one inode.\n");

  barrier_init(&start, 2);

  struct ConcurrentDeleteFileArgs* args = malloc(sizeof(struct ConcurrentDeleteFileArgs));
  assert(args != NULL, "ext_delete: failed to allocate busy-file worker args.\n");
  args->start = &start;
  args->ready_to_release = &ready_to_release;
  args->release = &release;
  args->done = &done;
  args->block_size = block_size;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "ext_delete: failed to allocate busy-file worker Fun.\n");
  fun->func = concurrent_delete_file_worker;
  fun->arg = args;
  thread(fun);

  barrier_sync(&start);
  node_delete(root, DELETE_TEST_BUSY_FILE_NAME);
  barrier_destroy(&start);

  deleted = node_find(root, DELETE_TEST_BUSY_FILE_NAME);
  assert(deleted == NULL,
    "ext_delete: busy-file pathname should disappear immediately after delete.\n");
  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks - 1,
    "ext_delete: busy-file delete must defer data-block reclamation until final release.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes - 1,
    "ext_delete: busy-file delete must defer inode reclamation until final release.\n");

  while (__atomic_load_n(&ready_to_release) == 0) {
    yield();
  }

  __atomic_fetch_add(&release, 1);

  while (__atomic_load_n(&done) == 0) {
    yield();
  }

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext_delete: busy-file resources should be reclaimed after the last wrapper releases.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext_delete: busy-file inode should be reclaimed after the last wrapper releases.\n");

  file = node_make_file(root, DELETE_TEST_BUSY_FILE_NAME);
  assert(file != NULL, "ext_delete: failed to recreate the busy delete file after release.\n");
  assert(file->cached->inumber == original_inumber,
    "ext_delete: final release should make the deleted file inode reusable.\n");
  node_free(file);
  node_delete(root, DELETE_TEST_BUSY_FILE_NAME);

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext_delete: busy-file recreate cleanup should restore the free block count.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext_delete: busy-file recreate cleanup should restore the free inode count.\n");

  say("***Concurrent busy-file delete: ok\n", NULL);
}

// Covers empty-directory deletion while another wrapper still points at the
// directory inode. The parent link count should drop immediately, but the
// directory inode and block must remain allocated until the last wrapper exits.
static void check_concurrent_delete_dir_coverage(struct Node* root) {
  unsigned baseline_blocks = root->filesystem->superblock.free_blocks_count;
  unsigned baseline_inodes = root->filesystem->superblock.free_inodes_count;
  unsigned baseline_root_links = node_get_num_links(root);
  int delete_done = 0;
  int ready_to_release = 0;
  int release = 0;
  int done = 0;
  struct Barrier start;
  struct Node* dir = node_make_dir(root, DELETE_TEST_BUSY_DIR_NAME);
  struct Node* deleted;

  assert(dir != NULL, "ext_delete: failed to create the busy delete directory.\n");
  node_free(dir);

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks - 1,
    "ext_delete: busy-dir setup should consume one data block.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes - 1,
    "ext_delete: busy-dir setup should consume one inode.\n");
  assert(node_get_num_links(root) == baseline_root_links + 1,
    "ext_delete: busy-dir setup should increment the parent link count.\n");

  barrier_init(&start, 2);

  struct ConcurrentDeleteDirArgs* args = malloc(sizeof(struct ConcurrentDeleteDirArgs));
  assert(args != NULL, "ext_delete: failed to allocate busy-dir worker args.\n");
  args->start = &start;
  args->delete_done = &delete_done;
  args->ready_to_release = &ready_to_release;
  args->release = &release;
  args->done = &done;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "ext_delete: failed to allocate busy-dir worker Fun.\n");
  fun->func = concurrent_delete_dir_worker;
  fun->arg = args;
  thread(fun);

  barrier_sync(&start);
  node_delete(root, DELETE_TEST_BUSY_DIR_NAME);
  barrier_destroy(&start);

  deleted = node_find(root, DELETE_TEST_BUSY_DIR_NAME);
  assert(deleted == NULL,
    "ext_delete: busy-dir pathname should disappear immediately after delete.\n");
  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks - 1,
    "ext_delete: busy-dir delete must defer block reclamation until final release.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes - 1,
    "ext_delete: busy-dir delete must defer inode reclamation until final release.\n");
  assert(node_get_num_links(root) == baseline_root_links,
    "ext_delete: busy-dir delete should restore the parent link count immediately.\n");

  __atomic_fetch_add(&delete_done, 1);

  while (__atomic_load_n(&ready_to_release) == 0) {
    yield();
  }

  __atomic_fetch_add(&release, 1);

  while (__atomic_load_n(&done) == 0) {
    yield();
  }

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext_delete: busy-dir resources should be reclaimed after the last wrapper releases.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext_delete: busy-dir inode should be reclaimed after the last wrapper releases.\n");
  assert(node_get_num_links(root) == baseline_root_links,
    "ext_delete: busy-dir final release should not perturb the parent link count.\n");

  dir = node_make_dir(root, DELETE_TEST_BUSY_DIR_NAME);
  assert(dir != NULL,
    "ext_delete: failed to recreate the busy delete directory after release.\n");
  node_free(dir);
  node_delete(root, DELETE_TEST_BUSY_DIR_NAME);

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext_delete: busy-dir recreate cleanup should restore the free block count.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext_delete: busy-dir recreate cleanup should restore the free inode count.\n");
  assert(node_get_num_links(root) == baseline_root_links,
    "ext_delete: busy-dir recreate cleanup should restore the parent link count.\n");

  say("***Concurrent busy-dir delete: ok\n", NULL);
}

// Run the concurrent delete coverage before the single-threaded accounting
// checks so inode-reuse assertions start from the pristine fixture image.
static void check_concurrent_delete_coverage(struct Node* root) {
  check_concurrent_delete_file_coverage(root);
  check_concurrent_delete_dir_coverage(root);
}

// Run the concurrent lifetime checks first, then the fixture and accounting coverage.
int kernel_main(void) {
  say("***Hello from ext2 delete test!\n", NULL);

  ext2_init(&fs);
  check_concurrent_delete_coverage(&fs.root);
  check_fixture_delete(&fs.root);
  check_dynamic_delete_coverage(&fs.root);

  ext2_destroy(&fs);

  return 0;
}
