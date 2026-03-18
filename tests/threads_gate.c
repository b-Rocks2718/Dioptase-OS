/*
 * Gate test.
 *
 * Validates:
 * - waiters block while the gate is closed
 * - signal wakes all current waiters and leaves the gate open for late arrivals
 * - reset closes the gate again so a new waiter set blocks until a later signal
 *
 * How:
 * - first stage queues INITIAL_WAITERS threads and verifies one signal wakes all
 * - a late waiter created after that signal must return without blocking
 * - after reset, a second waiter set must remain blocked until the next signal
 */

#include "../kernel/gate.h"
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

// Purpose: read the number of threads blocked in gate_wait().
// Inputs: none.
// Outputs: current waiter count.
// Preconditions: gate initialized.
// Postconditions: none.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static unsigned gate_waiter_count(void) {
  spin_lock_acquire(&gate.cv.lock);
  unsigned waiters = gate.cv.waiters;
  spin_lock_release(&gate.cv.lock);
  return waiters;
}

// Purpose: block on the global gate and record phase progress.
// Inputs: arg points to GateWaitArgs for this waiter.
// Preconditions: gate initialized; arg, ready, and done are non-NULL.
// Postconditions: increments *ready before waiting and *done after returning.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void gate_waiter_thread(void* arg) {
  struct GateWaitArgs* args = (struct GateWaitArgs*)arg;
  __atomic_fetch_add(args->ready, 1);
  gate_wait(&gate);
  __atomic_fetch_add(args->done, 1);
}

// Purpose: allocate and start one waiter thread for a specific test phase.
// Inputs: ready and done counters for the phase.
// Preconditions: gate initialized; ready and done are non-NULL.
// Postconditions: one new waiter thread has been scheduled.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
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

  gate_destroy(&gate);

  say("***gate ok\n", NULL);
  say("***gate test complete\n", NULL);
}
