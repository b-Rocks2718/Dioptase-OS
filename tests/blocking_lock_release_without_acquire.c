/*
 * Blocking lock release-without-acquire negative test.
 *
 * Validates:
 * - blocking_lock_release() rejects releasing an initialized lock that is not
 *   currently held.
 *
 * How:
 * - initialize a BlockingLock but intentionally do not acquire it
 * - call blocking_lock_release(); the expected result is a kernel panic before
 *   the semaphore is signaled or preemption state is restored
 */

#include "../kernel/blocking_lock.h"
#include "../kernel/print.h"

static struct BlockingLock lock;

void kernel_main(void) {
  say("***blocking lock release negative start\n", NULL);

  blocking_lock_init(&lock);
  blocking_lock_release(&lock);

  say("***blocking lock release negative FAIL\n", NULL);
}
