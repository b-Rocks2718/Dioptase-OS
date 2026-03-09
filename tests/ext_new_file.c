#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"

struct Ext2 fs;

int kernel_main(void) {
  say("***Hello from ext2 new file test!\n", NULL);

  ext2_init(&fs);

  struct Node* root = &fs.root;
  unsigned original_entry_count = node_entry_count(root);

  say("***Original root directory:\n", NULL);
  node_print_dir(root);

  struct Node* existing_file = ext2_make_file(&fs, root, "test.txt");
  assert(existing_file == NULL,
    "ext2_make_file: duplicate create should fail when the name already exists.\n");
  assert(node_entry_count(root) == original_entry_count,
    "ext2_make_file: duplicate create should not modify the parent directory.\n");

  struct Node* new_file = ext2_make_file(&fs, root, "new-file.txt");
  assert(new_file != NULL, "ext2_make_file: failed to create new file.\n");
  assert(node_is_file(new_file), "ext2_make_file: new file is not a regular file.\n");
  assert(node_entry_count(root) == original_entry_count + 1,
    "ext2_make_file: successful create should add exactly one directory entry.\n");
  node_free(new_file);

  struct Node* duplicate_new_file = ext2_make_file(&fs, root, "new-file.txt");
  assert(duplicate_new_file == NULL,
    "ext2_make_file: duplicate create should fail for a newly created file too.\n");
  assert(node_entry_count(root) == original_entry_count + 1,
    "ext2_make_file: failed duplicate create should leave the directory unchanged.\n");
  
  say("***New file creation: ok\n", NULL);

  say("***Root directory after creating new file:\n", NULL);
  node_print_dir(root);

  ext2_destroy(&fs);

  return 0;
}
