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
 *
 * How:
 * - create one regular file, one directory, nested files, and two symlinks
 *   while checking entry counts, link counts, and used_dirs_count
 * - reopen the symlinks and compare their stored targets
 * - race several workers to create the same basename behind one barrier and verify
 *   that only one directory entry is added
 */
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

struct Ext2 fs;
#define FAST_SYMLINK_TARGET_BYTES 60
#define FAST_SYMLINK_TARGET_BUFFER_BYTES 61
#define LONG_SYMLINK_TARGET_BYTES 64
#define LONG_SYMLINK_TARGET_BUFFER_BYTES 65
#define CONCURRENT_CREATE_WORKERS 8
#define CONCURRENT_CREATE_NAME "concurrent-create.txt"

static struct Barrier concurrent_create_start_barrier;
static int concurrent_create_finished = 0;
static int concurrent_create_successes = 0;

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

// Run the create suite across regular files, directories, symlinks, and the duplicate race.
int kernel_main(void) {
  say("***Hello from ext2 new file test!\n", NULL);

  ext2_init(&fs);

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

  check_concurrent_duplicate_create(root, original_entry_count + 4);

  say("***Root directory after create operations:\n", NULL);
  node_print_dir(root);

  say("***New directory after creating nested file:\n", NULL);
  node_print_dir(new_dir);

  node_free(new_dir);

  ext2_destroy(&fs);

  return 0;
}
