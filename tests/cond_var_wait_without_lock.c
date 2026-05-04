/*
 * Condition variable wait-without-lock negative test.
 *
 * Validates:
 * - cond_var_wait() rejects callers that do not hold the external BlockingLock
 *   protecting the condition predicate.
 *
 * How:
 * - initialize the CondVar and its external BlockingLock
 * - intentionally skip blocking_lock_acquire()
 * - call cond_var_wait(); the expected result is a kernel panic before the
 *   waiter is published or the external lock is released
 */

#include "../kernel/cond_var.h"
#include "../kernel/blocking_lock.h"
#include "../kernel/print.h"

static struct CondVar cv;
static struct BlockingLock lock;

void kernel_main(void) {
  say("***cond var wait negative start\n", NULL);

  cond_var_init(&cv);
  blocking_lock_init(&lock);
  cond_var_wait(&cv, &lock);

  say("***cond var wait negative FAIL\n", NULL);
}
