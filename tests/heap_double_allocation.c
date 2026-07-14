/*
 * Slab heap double-allocation bitmap negative test.
 *
 * Validates:
 * - HEAP_DEBUG allocation bitmap state rejects handing out an object whose
 *   allocation bit is already set.
 *
 * How:
 * - allocate one 32-byte object normally, leaving its allocation bitmap bit set
 * - pin the current thread and disable preemption while intentionally inserting
 *   that live object back into this core's per-core free list
 * - allocate another 32-byte object on the same pinned core; the expected result
 *   is a kernel panic when bitmap_alloc() sees the object is already live
 *
 * This test deliberately corrupts allocator-internal state to exercise a debug
 * detector that ordinary public allocator calls should never reach.
 */

#include "../kernel/heap.h"
#include "../kernel/per_core.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

void kernel_main(void) {
  say("***slab heap double allocation negative start\n", NULL);

  heap_init();
  void* obj = malloc(32);

  int core_was = core_pin();
  int preempt_was = preemption_disable();
  struct PerCore* core = get_per_core();

  int size_class = 0;
  while (size_class < NUM_OBJECT_SIZES && 32 > OBJECT_SIZES[size_class]) {
    size_class++;
  }
  assert(size_class < NUM_OBJECT_SIZES,
    "slab heap double allocation negative: size class not found.\n");

  ((struct FreeObject*)obj)->next = core->free_lists[size_class];
  core->free_lists[size_class] = obj;
  core->free_list_sizes[size_class]++;

  preemption_restore(preempt_was);

  malloc(32);

  core_unpin(core_was);
  say("***slab heap double allocation negative FAIL\n", NULL);
}
