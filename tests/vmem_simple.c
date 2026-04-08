#include "../kernel/vmem.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/heap.h"
#include "../kernel/physmem.h"
#include "../kernel/ext.h"

struct Ext2 fs;

void private_anonymous_test(void){
  int* p = mmap(FRAME_SIZE, false, NULL, 0);
  say("***    mmap'd a page at virtual address 0x%X\n", &p);

  p[0] = 42;
  say("***    wrote %d to first int of mmap'd page\n", &p[0]);

  int x = p[0];
  say("***    read %d from first int of mmap'd page\n", &x);

  munmap(p);
  say("***    munmap'd the page\n", NULL);
}

void private_file_backed_test(void){
  struct Node* file = node_find(&fs.root, "hello.txt");
  assert(file != NULL, "could not find hello.txt in ext2 filesystem\n");

  char* p = mmap(FRAME_SIZE, false, file, 0);
  say("***    mmap'd a file-backed page at virtual address 0x%X\n", &p);

  say("***    contents of hello.txt: %s\n", &p);

  p[6] = '!';
  say("***    modified contents of hello.txt: %s\n", &p);

  munmap(p);
  say("***    munmap'd the file-backed page\n", NULL);

  node_free(file);
}

int kernel_main(void) {
  say("***Hello from vmem_simple test!\n", NULL);

  ext2_init(&fs);

  say("***Running private anonymous mmap test...\n", NULL);
  private_anonymous_test();

  say("***Running private file-backed mmap test...\n", NULL);
  private_file_backed_test();

  ext2_destroy(&fs);

  return 0;
}
