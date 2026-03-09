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

  say("***Original root directory:\n", NULL);
  node_print_dir(root);

  struct Node* new_file = ext2_make_file(&fs, root, "new-file.txt");
  assert(new_file != NULL, "ext2_make_file: failed to create new file.\n");
  assert(node_is_file(new_file), "ext2_make_file: new file is not a regular file.\n");
  node_free(new_file);
  
  say("***New file creation: ok\n", NULL);

  say("***Root directory after creating new file:\n", NULL);
  node_print_dir(root);

  ext2_destroy(&fs);

  return 0;
}