/*
 * block() while holding a spinlock negative test.
 *
 * Validates:
 * - block() rejects attempts to context switch away from a thread whose TCB
 *   records an active spinlock.
 *
 * How:
 * - acquire a SpinLock, which disables interrupts and marks the current TCB's
 *   CLH node as locked
 * - call block() using the saved pre-lock interrupt mask; the expected result
 *   is a kernel panic before context_switch() can move execution to the idle
 *   thread or run the completion callback
 */

#include "../kernel/atomic.h"
#include "../kernel/debug.h"
#include "../kernel/print.h"
#include "../kernel/threads.h"

static struct SpinLock lock;

static void block_callback_should_not_run(void* arg) {
  (void)arg;
  panic("threads block spinlock negative: block callback unexpectedly ran.\n");
}

void kernel_main(void) {
  say("***threads block spinlock negative start\n", NULL);

  spin_lock_init(&lock);
  spin_lock_acquire(&lock);
  block(lock.interrupt_state, block_callback_should_not_run, NULL, true);

  say("***threads block spinlock negative FAIL\n", NULL);
}
