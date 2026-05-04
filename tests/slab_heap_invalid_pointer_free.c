/*
 * Slab heap invalid-pointer negative test.
 *
 * Validates:
 * - HEAP_DEBUG rejects pointers outside the physical-frame slab heap range
 *   before dereferencing candidate slab metadata.
 *
 * How:
 * - initialize the slab heap
 * - pass a small non-null pointer that cannot point into the physical-frame pool
 * - the expected result is a kernel panic from the slab free sanity check
 */

#include "../kernel/slab_heap.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap invalid pointer negative start\n", NULL);

  slab_heap_init();
  slab_heap_free((void*)4);

  say("***slab heap invalid pointer negative FAIL\n", NULL);
}
