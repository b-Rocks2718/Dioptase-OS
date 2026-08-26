/*
 * CLH nested spinlock negative test.
 *
 * Validates:
 * - clh_lock_acquire() rejects acquiring a CLH lock while the current thread
 *   already holds any spinlock through its per-TCB CLH node.
 *
 * How:
 * - acquire a normal SpinLock, which marks the current TCB's CLH node locked
 * - attempt to acquire a CLHLock; the expected result is a kernel panic before
 *   the CLH queue is mutated
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"

static struct SpinLock normal_lock;
static struct CLHLock clh_lock;

void kernel_main(void) {
  say("***clh nested negative start\n", NULL);

  spin_lock_init(&normal_lock);
  clh_lock_init(&clh_lock);

  spin_lock_acquire(&normal_lock);
  clh_lock_acquire(&clh_lock);

  say("***clh nested negative FAIL\n", NULL);
}
