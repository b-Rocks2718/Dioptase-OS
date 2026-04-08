#include "../kernel/vmem.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/heap.h"
#include "../kernel/physmem.h"

int kernel_main(void) {
  say("***Hello from vmem_simple test!\n", NULL);

  int* p = mmap(FRAME_SIZE, false, NULL, 0);
  say("***mmap'd a page at virtual address %X\n", &p);

  p[0] = 42;
  say("***wrote %d to first int of mmap'd page\n", &p[0]);

  int x = p[0];
  say("***read %d from first int of mmap'd page\n", &x);

  munmap(p);
  say("***munmap'd the page\n", NULL);

  return 0;
}
