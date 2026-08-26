/*
 * Physical page allocator invalid-free negative test.
 *
 * Validates:
 * - physmem_free() rejects invalid order-0 page addresses before publishing the
 *   pointer into any per-core cache.
 *
 * How:
 * - pass a small non-null pointer outside the documented physical-frame pool
 * - the expected result is a kernel panic from physmem_free() validation
 */

#include "../kernel/physmem.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***physmem invalid free negative start\n", NULL);

  physmem_free((void*)4);

  say("***physmem invalid free negative FAIL\n", NULL);
}
