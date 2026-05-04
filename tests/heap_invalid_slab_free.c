/*
 * Heap raw-frame free negative test.
 *
 * Validates:
 * - free() rejects a frame-aligned physical page that is not tracked as a live
 *   large heap allocation before treating it as slab metadata.
 *
 * How:
 * - allocate a raw physical page directly from physmem instead of slab_heap
 * - pass the page address to free(); the expected result is a kernel panic
 *   before the raw page is inserted into any heap freelist
 */

#include "../kernel/heap.h"
#include "../kernel/physmem.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap invalid slab negative start\n", NULL);

  heap_init();
  unsigned* page = (unsigned*)physmem_alloc();

  free(page);

  say("***slab heap invalid slab negative FAIL\n", NULL);
}
