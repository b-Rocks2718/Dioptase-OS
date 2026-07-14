/*
 * Spin lock release-without-acquire negative test.
 *
 * Validates:
 * - spin_lock_release() rejects release attempts by a thread whose TCB does
 *   not record any currently held spinlock.
 *
 * How:
 * - initialize a SpinLock but intentionally do not acquire it
 * - call spin_lock_release(); the expected result is a kernel panic before the
 *   lock word or interrupt state is modified
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"

static struct SpinLock lock;

void kernel_main(void) {
  say("***spin lock release negative start\n", NULL);

  spin_lock_init(&lock);
  spin_lock_release(&lock);

  say("***spin lock release negative FAIL\n", NULL);
}
