/*
 * ext2 create test.
 *
 * Validates:
 * - regular-file, directory, and symlink creates return the right inode type
 *   and update directory metadata correctly
 * - regular-file and symlink creates do not perturb used_dirs_count
 * - the fast-symlink inline boundary and a longer block-backed symlink target
 *   both round-trip correctly
 * - two concurrent creates of the same basename produce exactly one winner
 * - 255-byte basenames round-trip, while 256-byte regular-file, directory, and
 *   symlink names fail before consuming filesystem capacity
 * - inode/block summary exhaustion returns NULL from create helpers without
 *   publishing a parent entry, and creates recover after free counts restore
 * - a concurrent lookup can only observe fully initialized directories and
 *   symlinks, never the pre-publication construction state
 * - getdents retries a live entry that did not fit and rejects offsets inside
 *   an ext2 directory record
 *
 * How:
 * - create one regular file, one directory, nested files, and two symlinks
 *   while checking entry counts, link counts, and used_dirs_count
 * - reopen the symlinks and compare their stored targets
 * - race several workers to create the same basename behind one barrier and verify
 *   that only one directory entry is added
 * - create/delete one maximum-length name, then reject each create kind with
 *   an overlong name while checking free inode/block and entry counts
 * - race a lookup worker against a directory and a multi-block symlink create;
 *   every reachable snapshot must already contain the complete metadata/data
 * - read `.` and `..` one-at-a-time to verify the returned continuation offset
 */
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

#define FAST_SYMLINK_TARGET_BYTES 60
#define FAST_SYMLINK_TARGET_BUFFER_BYTES 61
#define LONG_SYMLINK_TARGET_BYTES 64
#define LONG_SYMLINK_TARGET_BUFFER_BYTES 65
#define CONCURRENT_CREATE_WORKERS 8
#define CONCURRENT_CREATE_NAME "concurrent-create.txt"
#define PUBLICATION_DIR_NAME "publication-dir"
#define PUBLICATION_SYMLINK_NAME "publication-link"
#define PUBLICATION_TARGET_BLOCKS 4
#define PUBLICATION_TARGET_EXTRA_BYTES 17
#define PUBLICATION_PARTICIPANTS 2
#define NEW_DIRECTORY_MANDATORY_ENTRIES 2
#define DIRENT_ALIGNMENT_BYTES 4
#define DOT_ENTRY_NAME_BYTES 1
#define DOT_DOT_ENTRY_NAME_BYTES 2
#define DIRENT_INTERIOR_TEST_OFFSET 1
#define MAX_NAME_BUFFER_BYTES 256
#define OVERLONG_NAME_BYTES 256
#define OVERLONG_NAME_BUFFER_BYTES 257

static struct Barrier concurrent_create_start_barrier;
static int concurrent_create_finished = 0;
static int concurrent_create_successes = 0;
static struct Barrier publication_start_barrier;
static int publication_creator_done = 0;
static int publication_observer_done = 0;
static char* publication_target = NULL;
static unsigned publication_target_size = 0;

// Sum ext2 used_dirs_count across every block group.
static unsigned count_used_dirs(struct Ext2* fs) {
  unsigned count = 0;

  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    count += fs->bgd_table[i].used_dirs_count;
  }

  return count;
}

// Fill a symlink target buffer with deterministic printable bytes.
static void fill_target_pattern(char* dest, unsigned size) {
  char* alphabet = "0123456789abcdef";

  for (unsigned i = 0; i < size; ++i){
    dest[i] = alphabet[i % 16];
  }

  dest[size] = '\0';
}

// Fill one basename with deterministic non-separator bytes and terminate it.
static void fill_basename(char* dest, unsigned size) {
  for (unsigned i = 0; i < size; ++i){
    dest[i] = 'n';
  }
  dest[size] = '\0';
}

/*
 * Exercise the exact ext2 basename boundary before the rest of the create
 * suite mutates accounting. The three overlong calls must return before
 * create_inode(), so they cannot consume an inode, a directory block, or a
 * parent entry. The exact-limit file proves the limit is inclusive rather than
 * accidentally rejecting the largest representable name.
 */
static void check_basename_limits(struct Node* root) {
  unsigned baseline_blocks = root->filesystem->superblock.free_blocks_count;
  unsigned baseline_inodes = root->filesystem->superblock.free_inodes_count;
  unsigned baseline_entries = node_entry_count(root);
  char accepted[MAX_NAME_BUFFER_BYTES];
  char rejected[OVERLONG_NAME_BUFFER_BYTES];
  struct Node* node;

  fill_basename(accepted, EXT2_MAX_NAME_BYTES);
  fill_basename(rejected, OVERLONG_NAME_BYTES);

  node = node_make_file(root, accepted);
  assert(node != NULL,
    "node_make_file: rejected the valid 255-byte ext2 basename boundary.\n");
  node_free(node);

  node = node_find(root, accepted);
  assert(node != NULL && node_is_file(node),
    "node_find: failed to round-trip the valid 255-byte basename.\n");
  node_free(node);
  assert(node_delete_typed(root, accepted,
      NODE_DELETE_FILE_OR_SYMLINK) == 0,
    "node_delete_typed: failed to remove the 255-byte regular-file name.\n");

  assert(node_make_file(root, rejected) == NULL,
    "node_make_file: accepted a 256-byte basename.\n");
  assert(node_make_dir(root, rejected) == NULL,
    "node_make_dir: accepted a 256-byte basename.\n");
  assert(node_make_symlink(root, rejected, "target") == NULL,
    "node_make_symlink: accepted a 256-byte basename.\n");

  assert(root->filesystem->superblock.free_blocks_count == baseline_blocks,
    "ext2 create name validation changed free blocks after rejection.\n");
  assert(root->filesystem->superblock.free_inodes_count == baseline_inodes,
    "ext2 create name validation changed free inodes after rejection.\n");
  assert(node_entry_count(root) == baseline_entries,
    "ext2 create name validation changed the parent directory entries.\n");

  say("***Basename length limits: ok\n", NULL);
}

// Return the packed linux_dirent size used by ext.c for one name length.
static unsigned test_dirent_size(unsigned name_len) {
  unsigned size = sizeof(struct linux_dirent) + name_len + 1;
  unsigned remainder = size % DIRENT_ALIGNMENT_BYTES;

  if (remainder != 0){
    size += DIRENT_ALIGNMENT_BYTES - remainder;
  }

  return size;
}

// Verify that a reachable publication-test symlink is one coherent snapshot,
// including both its locked size and every target byte.
static void check_published_symlink(struct Node* link) {
  unsigned copied_size = 0;
  char* copied;

  assert(node_is_symlink(link),
    "node_make_symlink: publication observer found the wrong inode type.\n");

  copied = node_copy_symlink_target(link, &copied_size);
  assert(copied_size == publication_target_size,
    "node_make_symlink: target size became reachable before initialization completed.\n");
  for (unsigned i = 0; i < publication_target_size; ++i){
    assert(copied[i] == publication_target[i],
      "node_make_symlink: target bytes became reachable before initialization completed.\n");
  }
  assert(copied[publication_target_size] == 0,
    "node_copy_symlink_target: target snapshot is not NUL-terminated.\n");
  free(copied);
}

/*
 * Continuously traverse the parent namespace while the main thread constructs
 * a directory and a multi-block symlink. The parent directory lock is the
 * publication boundary: if either lookup succeeds, all child state must
 * already be durable and complete. The long target makes the pre-fix window
 * broad enough for repeated multicore stress without adding a production-only
 * test hook.
 */
static void publication_observer_thread(void* unused) {
  bool saw_dir = false;
  bool saw_symlink = false;
  (void)unused;

  barrier_sync(&publication_start_barrier);

  while (__atomic_load_n(&publication_creator_done) == 0 ||
      !saw_dir || !saw_symlink) {
    struct Node* dir = node_find(&fs.root, PUBLICATION_DIR_NAME);
    if (dir != NULL){
      assert(node_is_dir(dir),
        "node_make_dir: publication observer found the wrong inode type.\n");
      assert(node_entry_count(dir) == NEW_DIRECTORY_MANDATORY_ENTRIES,
        "node_make_dir: directory became reachable before '.' and '..' were initialized.\n");
      saw_dir = true;
      node_free(dir);
    }

    struct Node* link = node_find(&fs.root, PUBLICATION_SYMLINK_NAME);
    if (link != NULL){
      check_published_symlink(link);
      saw_symlink = true;
      node_free(link);
    }

    if (!saw_dir || !saw_symlink){
      yield();
    }
  }

  __atomic_store_n(&publication_observer_done, 1);
}

// Race namespace traversal against construction, then clean up both fixtures
// so the checked directory listing remains stable across ext2 block sizes.
static void check_concurrent_create_publication(struct Node* root) {
  unsigned block_size = ext2_get_block_size(root->filesystem);
  struct Node* dir;
  struct Node* link;

  publication_target_size = block_size * PUBLICATION_TARGET_BLOCKS +
    PUBLICATION_TARGET_EXTRA_BYTES;
  publication_target = malloc(publication_target_size + 1);
  assert(publication_target != NULL,
    "ext_new_file: failed to allocate the publication-test symlink target.\n");
  fill_target_pattern(publication_target, publication_target_size);

  publication_creator_done = 0;
  publication_observer_done = 0;
  barrier_init(&publication_start_barrier, PUBLICATION_PARTICIPANTS);

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL,
    "ext_new_file: failed to allocate the publication observer Fun.\n");
  fun->func = publication_observer_thread;
  fun->arg = NULL;
  thread(fun);

  barrier_sync(&publication_start_barrier);

  dir = node_make_dir(root, PUBLICATION_DIR_NAME);
  assert(dir != NULL,
    "node_make_dir: publication-test directory create failed.\n");
  link = node_make_symlink(root, PUBLICATION_SYMLINK_NAME, publication_target);
  assert(link != NULL,
    "node_make_symlink: publication-test symlink create failed.\n");

  __atomic_store_n(&publication_creator_done, 1);
  while (__atomic_load_n(&publication_observer_done) == 0){
    yield();
  }

  barrier_destroy(&publication_start_barrier);
  check_published_symlink(link);
  node_free(link);
  node_free(dir);

  assert(node_delete(root, PUBLICATION_SYMLINK_NAME) == 0,
    "node_delete: failed to clean up the publication-test symlink.\n");
  assert(node_delete(root, PUBLICATION_DIR_NAME) == 0,
    "node_delete: failed to clean up the publication-test directory.\n");

  free(publication_target);
  publication_target = NULL;
  publication_target_size = 0;

  say("***Concurrent create publication: ok\n", NULL);
}

// A buffer that holds exactly one packed dirent must leave the offset at the
// second live entry, not advance past it. Invalid non-boundary offsets fail.
static void check_directory_iteration_offsets(struct Node* dir) {
  unsigned buffer_size = test_dirent_size(DOT_DOT_ENTRY_NAME_BYTES);
  char* buffer = malloc(buffer_size);
  int first_offset = -1;
  int second_offset = -1;
  int rc;

  assert(buffer != NULL,
    "node_getdents: failed to allocate the one-entry test buffer.\n");

  rc = node_getdents(dir, 0, buffer, buffer_size, &first_offset);
  assert(rc == test_dirent_size(DOT_ENTRY_NAME_BYTES),
    "node_getdents: one-entry buffer did not emit exactly the '.' entry.\n");
  assert(streq(&((struct linux_dirent*)buffer)->d_name, "."),
    "node_getdents: first new-directory entry is not '.'.\n");
  assert(first_offset > 0 && first_offset < node_size_in_bytes(dir),
    "node_getdents: full buffer skipped the next live directory entry.\n");

  rc = node_getdents(dir, first_offset, buffer, buffer_size, &second_offset);
  assert(rc == test_dirent_size(DOT_DOT_ENTRY_NAME_BYTES),
    "node_getdents: continuation did not emit exactly the '..' entry.\n");
  assert(streq(&((struct linux_dirent*)buffer)->d_name, ".."),
    "node_getdents: second new-directory entry is not '..'.\n");
  assert((unsigned)second_offset == node_size_in_bytes(dir),
    "node_getdents: consuming '..' did not advance to directory EOF.\n");

  rc = node_getdents(dir, DIRENT_INTERIOR_TEST_OFFSET,
    buffer, buffer_size, &second_offset);
  assert(rc == -1,
    "node_getdents: offset inside an ext2 directory record was accepted.\n");
  rc = node_getdents(dir, node_size_in_bytes(dir) + 1,
    buffer, buffer_size, &second_offset);
  assert(rc == -1,
    "node_getdents: offset beyond directory EOF was accepted.\n");

  free(buffer);
  say("***Directory iteration offsets: ok\n", NULL);
}

// Race one worker to create the shared duplicate-create basename.
static void concurrent_duplicate_create_thread(void* unused) {
  struct Node* created;
  (void)unused;

  barrier_sync(&concurrent_create_start_barrier);

  // Exactly one worker should observe a successful create.
  created = node_make_file(&fs.root, CONCURRENT_CREATE_NAME);
  if (created != NULL) {
    __atomic_fetch_add(&concurrent_create_successes, 1);
    node_free(created);
  }

  __atomic_fetch_add(&concurrent_create_finished, 1);
}

// Several threads race to create the same basename in the same directory. Exactly
// one create may succeed, and the parent directory may gain only one entry.
static void check_concurrent_duplicate_create(struct Node* root, unsigned expected_entry_count) {
  struct Node* existing = node_find(root, CONCURRENT_CREATE_NAME);
  assert(existing == NULL,
    "node_make_file: concurrent duplicate-create fixture should not exist before the race.\n");

  concurrent_create_finished = 0;
  concurrent_create_successes = 0;
  barrier_init(&concurrent_create_start_barrier, CONCURRENT_CREATE_WORKERS + 1);

  for (unsigned i = 0; i < CONCURRENT_CREATE_WORKERS; ++i){
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "node_make_file: concurrent duplicate-create Fun allocation failed.\n");
    fun->func = concurrent_duplicate_create_thread;
    fun->arg = NULL;
    thread(fun);
  }

  barrier_sync(&concurrent_create_start_barrier);

  while (__atomic_load_n(&concurrent_create_finished) != CONCURRENT_CREATE_WORKERS) {
    yield();
  }

  barrier_destroy(&concurrent_create_start_barrier);

  assert(__atomic_load_n(&concurrent_create_successes) == 1,
    "node_make_file: concurrent duplicate create should allow exactly one winner.\n");
  assert(node_entry_count(root) == expected_entry_count + 1,
    "node_make_file: concurrent duplicate create should add exactly one directory entry.\n");

  existing = node_find(root, CONCURRENT_CREATE_NAME);
  assert(existing != NULL,
    "node_make_file: concurrent duplicate create should leave one visible file.\n");
  assert(node_is_file(existing),
    "node_make_file: concurrent duplicate create should leave a regular file.\n");
  assert(node_get_num_links(existing) == 1,
    "node_make_file: concurrent duplicate create should leave one file with one link.\n");
  node_free(existing);

  say("***Concurrent duplicate create: ok\n", NULL);
}

// Force summary free counts to zero so create/write observe capacity exhaustion
// without panicking, then restore the live counters for later tests.
static void check_capacity_exhaustion(struct Node* root) {
  unsigned saved_free_inodes = fs.superblock.free_inodes_count;
  unsigned saved_free_blocks = fs.superblock.free_blocks_count;
  unsigned* saved_group_free_inodes = malloc(sizeof(unsigned) * fs.num_block_groups);
  unsigned* saved_group_free_blocks = malloc(sizeof(unsigned) * fs.num_block_groups);
  unsigned entry_count_before = node_entry_count(root);
  unsigned used_dirs_before = count_used_dirs(&fs);

  assert(saved_group_free_inodes != NULL && saved_group_free_blocks != NULL,
    "capacity exhaustion: failed to allocate free-count snapshots.\n");

  blocking_lock_acquire(&fs.metadata_lock);
  for (unsigned i = 0; i < fs.num_block_groups; ++i){
    saved_group_free_inodes[i] = fs.bgd_table[i].free_inodes_count;
    saved_group_free_blocks[i] = fs.bgd_table[i].free_blocks_count;
    fs.bgd_table[i].free_inodes_count = 0;
    fs.bgd_table[i].free_blocks_count = 0;
  }
  fs.superblock.free_inodes_count = 0;
  fs.superblock.free_blocks_count = 0;
  blocking_lock_release(&fs.metadata_lock);

  assert(node_make_file(root, "capacity-exhausted-file") == NULL,
    "capacity exhaustion: regular-file create succeeded with no free inodes.\n");
  assert(node_make_dir(root, "capacity-exhausted-dir") == NULL,
    "capacity exhaustion: directory create succeeded with no free inodes.\n");
  assert(node_make_symlink(root, "capacity-exhausted-link", "target") == NULL,
    "capacity exhaustion: symlink create succeeded with no free inodes.\n");
  assert(node_entry_count(root) == entry_count_before,
    "capacity exhaustion: failed create published a parent directory entry.\n");
  assert(count_used_dirs(&fs) == used_dirs_before,
    "capacity exhaustion: failed create changed used_dirs_count.\n");

  blocking_lock_acquire(&fs.metadata_lock);
  for (unsigned i = 0; i < fs.num_block_groups; ++i){
    fs.bgd_table[i].free_inodes_count = saved_group_free_inodes[i];
    fs.bgd_table[i].free_blocks_count = saved_group_free_blocks[i];
  }
  fs.superblock.free_inodes_count = saved_free_inodes;
  fs.superblock.free_blocks_count = saved_free_blocks;
  blocking_lock_release(&fs.metadata_lock);

  free(saved_group_free_inodes);
  free(saved_group_free_blocks);

  struct Node* recovered = node_make_file(root, "capacity-recovered-file");
  assert(recovered != NULL,
    "capacity exhaustion: create did not recover after restoring free counts.\n");
  node_free(recovered);
  assert(node_delete(root, "capacity-recovered-file") == 0,
    "capacity exhaustion: failed to delete the recovery fixture file.\n");

  say("***Filesystem capacity exhaustion: ok\n", NULL);
}

// Run the create suite across regular files, directories, symlinks, and the duplicate race.
int kernel_main(void) {
  say("***Hello from ext2 new file test!\n", NULL);

  struct Node* root = &fs.root;
  unsigned original_entry_count = node_entry_count(root);
  unsigned original_root_links = node_get_num_links(root);
  unsigned original_used_dirs = count_used_dirs(&fs);
  char inline_symlink_target[FAST_SYMLINK_TARGET_BUFFER_BYTES];
  char block_symlink_target[LONG_SYMLINK_TARGET_BUFFER_BYTES];

  fill_target_pattern(inline_symlink_target, FAST_SYMLINK_TARGET_BYTES);
  fill_target_pattern(block_symlink_target, LONG_SYMLINK_TARGET_BYTES);

  say("***Original root directory:\n", NULL);
  node_print_dir(root);

  check_basename_limits(root);

  // Start with duplicate-create rejection on an existing fixture name.
  struct Node* existing_file = node_make_file(root, "test.txt");
  assert(existing_file == NULL,
    "node_make_file: duplicate create should fail when the name already exists.\n");
  assert(node_entry_count(root) == original_entry_count,
    "node_make_file: duplicate create should not modify the parent directory.\n");

  struct Node* new_file = node_make_file(root, "new-file.txt");
  assert(new_file != NULL, "node_make_file: failed to create new file.\n");
  assert(node_is_file(new_file), "node_make_file: new file is not a regular file.\n");
  assert(node_get_num_links(new_file) == 1,
    "node_make_file: new regular files should start with exactly one link.\n");
  assert(node_entry_count(root) == original_entry_count + 1,
    "node_make_file: successful create should add exactly one directory entry.\n");
  assert(count_used_dirs(&fs) == original_used_dirs,
    "node_make_file: regular-file create should not change ext2 used_dirs_count.\n");
  node_free(new_file);

  struct Node* duplicate_new_file = node_make_file(root, "new-file.txt");
  assert(duplicate_new_file == NULL,
    "node_make_file: duplicate create should fail for a newly created file too.\n");
  assert(node_entry_count(root) == original_entry_count + 1,
    "node_make_file: failed duplicate create should leave the directory unchanged.\n");
  
  say("***New file creation: ok\n", NULL);

  // Then cover directory metadata and nested creates.
  struct Node* new_dir = node_make_dir(root, "new-dir");
  assert(new_dir != NULL, "node_make_dir: failed to create new directory.\n");
  assert(node_is_dir(new_dir), "node_make_dir: new directory is not a directory.\n");
  assert(node_get_num_links(new_dir) == 2,
    "node_make_dir: a new directory should start with exactly '.' and the parent entry.\n");
  assert(node_entry_count(root) == original_entry_count + 2,
    "node_make_dir: successful create should add exactly one directory entry.\n");
  assert(node_get_num_links(root) == original_root_links + 1,
    "node_make_dir: creating a subdirectory should increment the parent's link count.\n");
  assert(count_used_dirs(&fs) == original_used_dirs + 1,
    "node_make_dir: directory create should increment ext2 used_dirs_count exactly once.\n");

  check_directory_iteration_offsets(new_dir);

  say("***New directory creation: ok\n", NULL);

  struct Node* nested_file = node_make_file(new_dir, "nested-file.txt");
  assert(nested_file != NULL, "node_make_file: failed to create nested file.\n");
  assert(node_is_file(nested_file), "node_make_file: nested file is not a regular file.\n");
  assert(node_get_num_links(nested_file) == 1,
    "node_make_file: nested regular files should start with exactly one link.\n");
  assert(node_get_num_links(new_dir) == 2,
    "node_make_file: adding a regular file should not change the parent directory link count.\n");
  assert(count_used_dirs(&fs) == original_used_dirs + 1,
    "node_make_file: nested regular-file create should not change ext2 used_dirs_count.\n");
  node_free(nested_file);
  say("***New file in new directory: ok\n", NULL);

  // Finish the sequential phase with inline and block-backed symlink targets.
  struct Node* inline_link = node_make_symlink(root, "inline-link", inline_symlink_target);
  assert(inline_link != NULL, "node_make_symlink: failed to create inline symlink.\n");
  assert(node_is_symlink(inline_link), "node_make_symlink: inline symlink did not create a symlink inode.\n");
  assert(node_get_num_links(inline_link) == 1,
    "node_make_symlink: new symlink inodes should start with exactly one link.\n");
  assert(node_entry_count(root) == original_entry_count + 3,
    "node_make_symlink: inline symlink create should add exactly one directory entry.\n");
  assert(count_used_dirs(&fs) == original_used_dirs + 1,
    "node_make_symlink: symlink create should not change ext2 used_dirs_count.\n");

  // A corrupt on-disk i_size of UINT_MAX cannot be incremented for the
  // required NUL terminator. Temporarily inject that metadata state and verify
  // the snapshot API fails without wrapping to malloc(0) or modifying the
  // caller's output value, then restore the live test inode before proceeding.
  unsigned saved_inline_size;
  unsigned malformed_target_size = 123;
  blocking_lock_acquire(&inline_link->cached->lock);
  saved_inline_size = inline_link->cached->inode.size;
  inline_link->cached->inode.size = UINT_MAX;
  blocking_lock_release(&inline_link->cached->lock);
  assert(node_copy_symlink_target(inline_link, &malformed_target_size) == NULL,
    "node_copy_symlink_target: accepted an unrepresentable UINT_MAX target size.\n");
  assert(malformed_target_size == 123,
    "node_copy_symlink_target: modified target_size on malformed metadata failure.\n");
  blocking_lock_acquire(&inline_link->cached->lock);
  inline_link->cached->inode.size = saved_inline_size;
  blocking_lock_release(&inline_link->cached->lock);

  char inline_target_buf[FAST_SYMLINK_TARGET_BUFFER_BYTES];
  node_get_symlink_target(inline_link, inline_target_buf);
  assert(streq(inline_target_buf, inline_symlink_target),
    "node_make_symlink: inline symlink target was not stored inline in the inode.\n");
  node_free(inline_link);

  struct Node* reopened_inline_link = node_find(root, "inline-link");
  assert(reopened_inline_link != NULL,
    "node_make_symlink: inline symlink could not be reopened from the directory.\n");
  assert(node_is_symlink(reopened_inline_link),
    "node_make_symlink: reopened inline symlink is not a symlink.\n");
  node_get_symlink_target(reopened_inline_link, inline_target_buf);
  assert(streq(inline_target_buf, inline_symlink_target),
    "node_make_symlink: reopened inline symlink target did not match the stored bytes.\n");
  node_free(reopened_inline_link);
  say("***Inline symlink creation: ok\n", NULL);

  struct Node* block_link = node_make_symlink(root, "block-link", block_symlink_target);
  assert(block_link != NULL, "node_make_symlink: failed to create block-backed symlink.\n");
  assert(node_is_symlink(block_link), "node_make_symlink: block-backed create did not produce a symlink inode.\n");
  assert(node_get_num_links(block_link) == 1,
    "node_make_symlink: block-backed symlink inodes should start with exactly one link.\n");
  assert(node_entry_count(root) == original_entry_count + 4,
    "node_make_symlink: block-backed symlink create should add exactly one directory entry.\n");
  assert(count_used_dirs(&fs) == original_used_dirs + 1,
    "node_make_symlink: block-backed symlink create should not change ext2 used_dirs_count.\n");
  char block_target_buf[LONG_SYMLINK_TARGET_BUFFER_BYTES];
  node_get_symlink_target(block_link, block_target_buf);
  assert(streq(block_target_buf, block_symlink_target),
    "node_make_symlink: block-backed symlink target was not written through the inode data blocks.\n");
  node_free(block_link);

  struct Node* reopened_block_link = node_find(root, "block-link");
  assert(reopened_block_link != NULL,
    "node_make_symlink: block-backed symlink could not be reopened from the directory.\n");
  assert(node_is_symlink(reopened_block_link),
    "node_make_symlink: reopened block-backed symlink is not a symlink.\n");
  node_get_symlink_target(reopened_block_link, block_target_buf);
  assert(streq(block_target_buf, block_symlink_target),
    "node_make_symlink: reopened block-backed symlink target did not match the stored bytes.\n");
  node_free(reopened_block_link);
  say("***Block-backed symlink creation: ok\n", NULL);

  check_capacity_exhaustion(root);

  check_concurrent_duplicate_create(root, original_entry_count + 4);

  say("***Root directory after create operations:\n", NULL);
  node_print_dir(root);

  say("***New directory after creating nested file:\n", NULL);
  node_print_dir(new_dir);

  check_concurrent_create_publication(root);

  node_free(new_dir);

  return 0;
}
