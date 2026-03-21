/*
 * Covers ext2 same-directory rename of one regular file.
 * Verifies the renamed entry keeps the same inode and contents and that the
 * old pathname is removed from the directory.
 */
#include "../kernel/ext.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/string.h"

struct Ext2 fs;

// Reads the full file contents and appends a trailing NUL so the rename test
// can validate the payload using string helpers.
static char* read_node_text(struct Node* node) {
  unsigned size = node_size_in_bytes(node);
  char* text = malloc(size + 1);
  assert(text != NULL, "ext_rename: failed to allocate a file read buffer.\n");

  unsigned cnt = node_read_all(node, 0, size, text);
  assert(cnt == size, "ext_rename: failed to read the full file contents.\n");
  text[size] = 0;

  return text;
}

int kernel_main(void){
  say("***ext_rename test start\n", NULL);

  ext2_init(&fs);

  struct Node* original = node_find(&fs.root, "hello.txt");
  assert(original != NULL, "ext_rename: hello.txt not found in the root directory.\n");

  unsigned original_inumber = original->cached->inumber;
  char* original_text = read_node_text(original);
  assert(streq(original_text, "test"),
    "ext_rename: hello.txt should contain the fixture contents before rename.\n");
  free(original_text);
  node_free(original);

  node_rename(&fs.root, "hello.txt", "goodbye.txt");

  struct Node* old_name = node_find(&fs.root, "hello.txt");
  assert(old_name == NULL,
    "ext_rename: old pathname should not resolve after rename.\n");

  struct Node* renamed = node_find(&fs.root, "goodbye.txt");
  assert(renamed != NULL,
    "ext_rename: new pathname not found after rename.\n");
  assert(renamed->cached->inumber == original_inumber,
    "ext_rename: rename should preserve the inode number.\n");

  char* renamed_text = read_node_text(renamed);
  assert(streq(renamed_text, "test"),
    "ext_rename: rename should preserve the file contents.\n");
  free(renamed_text);
  node_free(renamed);

  say("***Rename inode+content: ok\n", NULL);
  say("***Rename removes old name: ok\n", NULL);

  ext2_destroy(&fs);

  say("***ext_rename test complete\n", NULL);
  return 0;
}
