/*
 * Slab heap use-after-free poison negative test.
 *
 * Validates:
 * - HEAP_DEBUG poison checking detects writes to a freed object's payload before
 *   the object is handed out again from a per-core freelist.
 *
 * How:
 * - allocate and free one 32-byte object so it is cached on this core's freelist
 * - overwrite a non-link word after free; word 0 is intentionally excluded
 *   because the allocator owns it as the freelist next pointer
 * - allocate another 32-byte object; the per-core LIFO cache should return the
 *   same object and the poison check should panic before the caller receives it
 */

#include "../kernel/slab_heap.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap use after free negative start\n", NULL);

  slab_heap_init();
  unsigned* obj = (unsigned*)slab_heap_alloc(32);
  slab_heap_free(obj);

  obj[1] = 0x12345678;
  slab_heap_alloc(32);

  say("***slab heap use after free negative FAIL\n", NULL);
}
