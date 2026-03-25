/*
 * Condition variable test.
 *
 * Validates:
 * - cond_var_wait releases the external lock while blocked and re-acquires it
 *   before returning
 * - signal wakes exactly one waiter
 * - broadcast wakes the remaining waiters
 *
 * How:
 * - create NUM_WAITERS workers that wait under one external blocking lock
 * - first hand out one ticket and call signal
 * - then hand out the remaining tickets and call broadcast
 * - use the shared critical-section flag and waiter count to detect bad wakes
 */

#include "../kernel/cond_var.h"
#include "../kernel/blocking_lock.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_WAITERS 6

static struct CondVar cv;
static struct BlockingLock lock;

static int ready = 0;
static int done = 0;
static int tickets = 0;
static int in_critical = 0;

// Read the current waiter count straight from the condvar internals.
static unsigned cond_var_waiter_count(void) {
  spin_lock_acquire(&cv.lock);
  unsigned n = cv.waiters;
  spin_lock_release(&cv.lock);
  return n;
}

// Wait for a ticket, then enter the protected critical section exactly once.
static void waiter_thread(void* arg) {
  int id = *(int*)arg;
  (void)id;

  blocking_lock_acquire(&lock);
  __atomic_fetch_add(&ready, 1);

  // Wait until main grants this thread a wakeup ticket.
  while (tickets == 0) {
    cond_var_wait(&cv, &lock);
  }

  tickets -= 1;

  // The external lock should be back in place when wait returns.
  if (in_critical != 0) {
    say("***cond_var FAIL waiter entered critical section concurrently\n", NULL);
    panic("cond_var test: external lock was not held after wait\n");
  }
  in_critical = 1;
  __atomic_fetch_add(&done, 1);
  in_critical = 0;

  blocking_lock_release(&lock);
}

// Drive the signal-then-broadcast sequence and verify the wakeup counts.
void kernel_main(void) {
  say("***cond_var test start\n", NULL);

  cond_var_init(&cv);
  blocking_lock_init(&lock);

  blocking_lock_acquire(&lock);

  // Empty wakeups should not poison later waits.
  cond_var_signal(&cv, &lock);
  cond_var_broadcast(&cv, &lock);

  blocking_lock_release(&lock);

  // Start the waiter set that will block on the condvar.
  for (int i = 0; i < NUM_WAITERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "cond_var test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "cond_var test: Fun allocation failed.\n");
    fun->func = waiter_thread;
    fun->arg = id;
    thread(fun);
  }

  while (__atomic_load_n(&ready) != NUM_WAITERS) {
    yield();
  }

  while (cond_var_waiter_count() != NUM_WAITERS) {
    yield();
  }

  // First wake exactly one waiter.
  blocking_lock_acquire(&lock);
  tickets = 1;
  cond_var_signal(&cv, &lock);
  blocking_lock_release(&lock);

  while (__atomic_load_n(&done) != 1) {
    yield();
  }

  for (int i = 0; i < 50; i++) {
    yield();
  }
  if (__atomic_load_n(&done) != 1) {
    int args[2] = { __atomic_load_n(&done), 1 };
    say("***cond_var FAIL after signal done=%d expected=%d\n", args);
    panic("cond_var test: signal woke incorrect number of waiters\n");
  }

  // Then wake the rest in one broadcast.
  blocking_lock_acquire(&lock);
  tickets = NUM_WAITERS - 1;
  cond_var_broadcast(&cv, &lock);
  blocking_lock_release(&lock);

  while (__atomic_load_n(&done) != NUM_WAITERS) {
    yield();
  }

  if (cond_var_waiter_count() != 0) {
    int args[2] = { (int)cond_var_waiter_count(), 0 };
    say("***cond_var FAIL waiters=%d expected=%d\n", args);
    panic("cond_var test: waiter count mismatch after broadcast\n");
  }

  say("***cond_var ok\n", NULL);
  say("***cond_var test complete\n", NULL);
}
