/*
 * Spin barrier while holding a spinlock negative test.
 *
 * Validates:
 * - spin_barrier_sync() rejects callers that already hold a spinlock, because
 *   spin-barrier waits can last longer than a bounded spinlock critical
 *   section and would keep interrupts disabled while spinning.
 *
 * How:
 * - acquire a SpinLock, which marks the current TCB's CLH node as locked
 * - call spin_barrier_sync(); the expected result is a kernel panic before the
 *   barrier counter is decremented
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"

static struct SpinLock lock;
static int barrier = 1;

void kernel_main(void) {
  say("***spin barrier negative start\n", NULL);

  spin_lock_init(&lock);
  spin_lock_acquire(&lock);
  spin_barrier_sync(&barrier);

  say("***spin barrier negative FAIL\n", NULL);
}
