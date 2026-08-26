/*
 * Slab heap double-free negative test.
 *
 * Validates:
 * - HEAP_DEBUG allocation bitmap state rejects freeing an object whose public
 *   allocation bit is already clear.
 *
 * How:
 * - initialize the slab heap and allocate one 32-byte object
 * - free it once, which clears its allocation bit and poisons its payload
 * - free the same pointer again; the expected result is a kernel panic from the
 *   allocation bitmap before the second free can return as a valid lifecycle
 *   transition
 */

#include "../kernel/heap.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap double free negative start\n", NULL);

  heap_init();
  void* obj = malloc(32);
  free(obj);
  free(obj);

  say("***slab heap double free negative FAIL\n", NULL);
}
