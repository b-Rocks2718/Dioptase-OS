/*
 * Gate test.
 *
 * Validates:
 * - waiters block while the gate is closed
 * - signal wakes all current waiters and leaves the gate open for late arrivals
 * - reset closes the gate again so a new waiter set blocks until a later signal
 * - gate_destroy() reaps waiters blocked on the gate's internal blocking lock
 *
 * How:
 * - first stage queues INITIAL_WAITERS threads and verifies one signal wakes all
 * - a late waiter created after that signal must return without blocking
 * - after reset, a second waiter set must remain blocked until the next signal
 * - finally hold the internal lock directly, queue one gate_wait() caller on
 *   that lock, destroy the gate, and verify the waiter never resumes
 */

#include "../kernel/gate.h"
#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define INITIAL_WAITERS 6
#define RESET_WAITERS 4
#define READY_WAIT_BUDGET 100000
#define WAITER_WAIT_BUDGET 100000
#define DONE_WAIT_BUDGET 100000
#define IMMEDIATE_RETURN_YIELDS 128
#define BLOCKED_CHECK_YIELDS 128

struct GateWaitArgs {
  int* ready;
  int* done;
};

static struct Gate gate;
static int initial_ready = 0;
static int initial_done = 0;
static int late_ready = 0;
static int late_done = 0;
static int reset_ready = 0;
static int reset_done = 0;
static struct Semaphore holder_release_sem;
static int holder_ready = 0;
static int holder_done = 0;
static int destroy_waiter_started = 0;
static int destroy_waiter_done = 0;

// Read the number of threads currently blocked in gate_wait().
static unsigned gate_waiter_count(void) {
  spin_lock_acquire(&gate.cv.lock);
  unsigned waiters = gate.cv.waiters;
  spin_lock_release(&gate.cv.lock);
  return waiters;
}

// Read the number of threads blocked on the gate's internal blocking lock.
static unsigned gate_lock_waiter_count(void) {
  spin_lock_acquire(&gate.lock.semaphore.lock);
  unsigned waiters = gate.lock.semaphore.wait_queue.size;
  spin_lock_release(&gate.lock.semaphore.lock);
  return waiters;
}

// Wait on the shared gate once and record when this waiter starts and finishes.
static void gate_waiter_thread(void* arg) {
  struct GateWaitArgs* args = (struct GateWaitArgs*)arg;
  __atomic_fetch_add(args->ready, 1);
  gate_wait(&gate);
  __atomic_fetch_add(args->done, 1);
}

// Reserve gate.lock's semaphore directly so another thread blocks in
// blocking_lock_acquire() without this helper inheriting blocking-lock
// preemption semantics it will never release.
static void lock_holder_thread(void* arg) {
  (void)arg;
  sem_down(&gate.lock.semaphore);
  __atomic_store_n(&holder_ready, 1);
  sem_down(&holder_release_sem);
  // gate_destroy() destroys the lock, so this thread must not release it.
  __atomic_store_n(&holder_done, 1);
}

// Attempt one gate_wait() while another thread holds the internal lock.
static void destroy_waiter_thread(void* arg) {
  (void)arg;
  __atomic_store_n(&destroy_waiter_started, 1);
  gate_wait(&gate);
  __atomic_store_n(&destroy_waiter_done, 1);
}

// Allocate and start one waiter for the requested gate phase.
static void spawn_waiter(int* ready, int* done) {
  struct GateWaitArgs* args = malloc(sizeof(struct GateWaitArgs));
  assert(args != NULL, "gate test: waiter arg allocation failed.\n");
  args->ready = ready;
  args->done = done;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "gate test: Fun allocation failed.\n");
  fun->func = gate_waiter_thread;
  fun->arg = args;
  thread(fun);
}

void kernel_main(void) {
  say("***gate test start\n", NULL);

  gate_init(&gate);
  sem_init(&holder_release_sem, 0);

  // First prove the closed gate blocks every waiter until signal().
  for (int i = 0; i < INITIAL_WAITERS; i++) {
    spawn_waiter(&initial_ready, &initial_done);
  }

  for (int i = 0; i < READY_WAIT_BUDGET &&
                  __atomic_load_n(&initial_ready) != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&initial_ready) != INITIAL_WAITERS) {
    int args[2] = { __atomic_load_n(&initial_ready), INITIAL_WAITERS };
    say("***gate FAIL initial ready=%d expected=%d\n", args);
    panic("gate test: initial waiters did not start\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET &&
                  gate_waiter_count() != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (gate_waiter_count() != INITIAL_WAITERS) {
    int args[2] = { (int)gate_waiter_count(), INITIAL_WAITERS };
    say("***gate FAIL initial waiters=%d expected=%d\n", args);
    panic("gate test: initial waiters did not block on the closed gate\n");
  }

  gate_signal(&gate);

  for (int i = 0; i < DONE_WAIT_BUDGET &&
                  __atomic_load_n(&initial_done) != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&initial_done) != INITIAL_WAITERS) {
    int args[2] = { __atomic_load_n(&initial_done), INITIAL_WAITERS };
    say("***gate FAIL initial done=%d expected=%d\n", args);
    panic("gate test: signal did not release all initial waiters\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET && gate_waiter_count() != 0; i++) {
    yield();
  }
  if (gate_waiter_count() != 0) {
    int args[2] = { (int)gate_waiter_count(), 0 };
    say("***gate FAIL waiters after first signal=%d expected=%d\n", args);
    panic("gate test: waiters remained queued after signal\n");
  }

  spawn_waiter(&late_ready, &late_done);

  for (int i = 0; i < READY_WAIT_BUDGET && __atomic_load_n(&late_ready) != 1; i++) {
    yield();
  }
  if (__atomic_load_n(&late_ready) != 1) {
    int args[2] = { __atomic_load_n(&late_ready), 1 };
    say("***gate FAIL late ready=%d expected=%d\n", args);
    panic("gate test: late waiter did not start\n");
  }

  for (int i = 0; i < IMMEDIATE_RETURN_YIELDS &&
                  __atomic_load_n(&late_done) != 1;
       i++) {
    yield();
  }
  if (__atomic_load_n(&late_done) != 1) {
    int args[2] = { __atomic_load_n(&late_done), 1 };
    say("***gate FAIL late done=%d expected=%d\n", args);
    panic("gate test: wait after signal blocked even though the gate remained open\n");
  }

  // Reset should close the gate again for the next batch of waiters.
  gate_reset(&gate);

  for (int i = 0; i < RESET_WAITERS; i++) {
    spawn_waiter(&reset_ready, &reset_done);
  }

  for (int i = 0; i < READY_WAIT_BUDGET &&
                  __atomic_load_n(&reset_ready) != RESET_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&reset_ready) != RESET_WAITERS) {
    int args[2] = { __atomic_load_n(&reset_ready), RESET_WAITERS };
    say("***gate FAIL reset ready=%d expected=%d\n", args);
    panic("gate test: reset-phase waiters did not start\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET &&
                  gate_waiter_count() != RESET_WAITERS;
       i++) {
    yield();
  }
  if (gate_waiter_count() != RESET_WAITERS) {
    int args[2] = { (int)gate_waiter_count(), RESET_WAITERS };
    say("***gate FAIL reset waiters=%d expected=%d\n", args);
    panic("gate test: gate_reset did not close the gate for new waiters\n");
  }

  for (int i = 0; i < BLOCKED_CHECK_YIELDS; i++) {
    yield();
  }
  if (__atomic_load_n(&reset_done) != 0) {
    int args[2] = { __atomic_load_n(&reset_done), 0 };
    say("***gate FAIL reset done=%d expected=%d\n", args);
    panic("gate test: reset-phase waiters passed through before the second signal\n");
  }

  // The second signal should release the reset-phase waiters as one group.
  gate_signal(&gate);

  for (int i = 0; i < DONE_WAIT_BUDGET &&
                  __atomic_load_n(&reset_done) != RESET_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&reset_done) != RESET_WAITERS) {
    int args[2] = { __atomic_load_n(&reset_done), RESET_WAITERS };
    say("***gate FAIL second done=%d expected=%d\n", args);
    panic("gate test: second signal did not release all reset-phase waiters\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET && gate_waiter_count() != 0; i++) {
    yield();
  }
  if (gate_waiter_count() != 0) {
    int args[2] = { (int)gate_waiter_count(), 0 };
    say("***gate FAIL waiters after second signal=%d expected=%d\n", args);
    panic("gate test: reset-phase waiters remained queued after signal\n");
  }

  // Destroy must also reap waiters parked on the embedded blocking lock.
  gate_reset(&gate);

  struct Fun* holder_fun = malloc(sizeof(struct Fun));
  assert(holder_fun != NULL, "gate test: holder Fun allocation failed.\n");
  holder_fun->func = lock_holder_thread;
  holder_fun->arg = NULL;
  thread(holder_fun);

  for (int i = 0; i < READY_WAIT_BUDGET && __atomic_load_n(&holder_ready) != 1; i++) {
    yield();
  }
  if (__atomic_load_n(&holder_ready) != 1) {
    say("***gate FAIL holder did not acquire internal lock\n", NULL);
    panic("gate test: holder thread did not acquire gate lock\n");
  }

  struct Fun* destroy_fun = malloc(sizeof(struct Fun));
  assert(destroy_fun != NULL, "gate test: destroy waiter Fun allocation failed.\n");
  destroy_fun->func = destroy_waiter_thread;
  destroy_fun->arg = NULL;
  thread(destroy_fun);

  for (int i = 0; i < READY_WAIT_BUDGET &&
                  __atomic_load_n(&destroy_waiter_started) != 1;
       i++) {
    yield();
  }
  if (__atomic_load_n(&destroy_waiter_started) != 1) {
    say("***gate FAIL destroy waiter did not start\n", NULL);
    panic("gate test: destroy waiter did not start\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET && gate_lock_waiter_count() != 1; i++) {
    yield();
  }
  if (gate_lock_waiter_count() != 1) {
    int args[2] = { (int)gate_lock_waiter_count(), 1 };
    say("***gate FAIL lock waiters=%d expected=%d\n", args);
    panic("gate test: destroy waiter did not block on the internal lock\n");
  }

  gate_destroy(&gate);

  for (int i = 0; i < BLOCKED_CHECK_YIELDS; i++) {
    yield();
  }

  if (gate_lock_waiter_count() != 0) {
    int args[2] = { (int)gate_lock_waiter_count(), 0 };
    say("***gate FAIL lock waiters after destroy=%d expected=%d\n", args);
    panic("gate test: gate_destroy left lock waiters queued\n");
  }
  if (__atomic_load_n(&destroy_waiter_done) != 0) {
    int args[2] = { __atomic_load_n(&destroy_waiter_done), 0 };
    say("***gate FAIL destroy waiter done=%d expected=%d\n", args);
    panic("gate test: destroyed gate waiter resumed from internal lock wait\n");
  }

  sem_up(&holder_release_sem);
  for (int i = 0; i < DONE_WAIT_BUDGET && __atomic_load_n(&holder_done) != 1; i++) {
    yield();
  }
  if (__atomic_load_n(&holder_done) != 1) {
    say("***gate FAIL holder did not exit after destroy\n", NULL);
    panic("gate test: holder thread did not exit after destroy\n");
  }

  say("***gate ok\n", NULL);
  say("***gate test complete\n", NULL);
}
