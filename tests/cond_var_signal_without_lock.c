/*
 * Condition variable signal-without-lock negative test.
 *
 * Validates:
 * - cond_var_signal() rejects callers that do not hold the external
 *   BlockingLock protecting the condition predicate and wakeup ordering.
 *
 * How:
 * - initialize the CondVar and its external BlockingLock
 * - intentionally skip blocking_lock_acquire()
 * - call cond_var_signal(); the expected result is a kernel panic before the
 *   wait queue is inspected
 */

#include "../kernel/cond_var.h"
#include "../kernel/blocking_lock.h"
#include "../kernel/print.h"

static struct CondVar cv;
static struct BlockingLock lock;

void kernel_main(void) {
  say("***cond var signal negative start\n", NULL);

  cond_var_init(&cv);
  blocking_lock_init(&lock);
  cond_var_signal(&cv, &lock);

  say("***cond var signal negative FAIL\n", NULL);
}
