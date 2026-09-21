/*
 * Preemption-spin-lock identity negative test.
 *
 * Preemption state is core-local and these locks are available before TCB
 * bootstrap, so ownership is represented by core ID. A core holding one lock
 * must not use that fact to release a distinct object.
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static struct PreemptSpinLock owned;
static struct PreemptSpinLock other;

void kernel_main(void){ /* Verify a preemption lock cannot be released by another core. */
  say("***preemption-spin-lock wrong-release negative start\n", NULL);
  preempt_spin_lock_init(&owned);
  preempt_spin_lock_init(&other);
  preempt_spin_lock_acquire(&owned);
  preempt_spin_lock_release(&other);
  panic("preemption-spin-lock wrong-release test: release unexpectedly succeeded.\n");
}
