/*
 * Slab heap interior-pointer negative test.
 *
 * Validates:
 * - HEAP_DEBUG rejects a pointer inside a valid slab object when it is not
 *   aligned to that slab's object size.
 *
 * How:
 * - allocate one 32-byte object from a valid slab
 * - add four bytes to keep the pointer word-aligned but move it inside the
 *   object rather than at an object boundary
 * - free the interior pointer; the expected result is a kernel panic before
 *   any freelist or bitmap state is updated for the bad address
 */

#include "../kernel/heap.h"
#include "../kernel/print.h"

void kernel_main(void) {
  say("***slab heap interior pointer negative start\n", NULL);

  heap_init();
  char* obj = (char*)malloc(32);
  free(obj + 4);

  say("***slab heap interior pointer negative FAIL\n", NULL);
}
