/*
 * Slab heap invalid-slab-metadata negative test.
 *
 * Validates:
 * - HEAP_DEBUG rejects a frame-range pointer whose containing frame is not a
 *   slab with one of the supported object sizes.
 *
 * How:
 * - allocate a raw physical page directly from physmem instead of slab_heap
 * - stamp the word where struct Slab::object_size would live with an unsupported
 *   size so the test does not depend on leftover page contents
 * - pass the page address to slab_heap_free(); the expected result is a kernel
 *   panic before the raw page is inserted into any slab freelist
 */

#include "../kernel/heap.h"
#include "../kernel/physmem.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap invalid slab negative start\n", NULL);

  heap_init();
  unsigned* page = (unsigned*)physmem_alloc();
  page[1] = 12345;

  free(page);

  say("***slab heap invalid slab negative FAIL\n", NULL);
}
