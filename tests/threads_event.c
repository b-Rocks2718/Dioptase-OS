/*
 * Event test.
 *
 * Validates:
 * - signal wakes all waiters currently blocked in event_wait()
 * - a past signal is not sticky for future waiters
 * - the same event can be reused across many generations without a stale signal
 *   leaking into the next wait round
 *
 * How:
 * - first stage queues INITIAL_WAITERS threads and verifies one signal wakes all
 * - second stage keeps REUSE_WAITERS threads looping over EVENT_ROUNDS waits
 * - worker 0 yields after every wake so the other workers start the next
 *   event_wait() before the previous round's bookkeeping is complete
 * - each worker checks that all round waiters were present when it woke; a stale
 *   signal would let a future waiter resume before that count reaches REUSE_WAITERS
 */

#include "../kernel/event.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define INITIAL_WAITERS 6
#define REUSE_WAITERS 4
#define EVENT_ROUNDS 16
#define SLOW_REUSE_WAITER_ID 0
#define POST_SIGNAL_SLOW_YIELDS 64
#define READY_WAIT_BUDGET 100000
#define WAITER_WAIT_BUDGET 100000
#define DONE_WAIT_BUDGET 100000

struct EventWaitArgs {
  int* ready;
  int* done;
};

static struct Event event;
static int initial_ready = 0;
static int initial_done = 0;
static int reuse_started = 0;
static int reuse_finished = 0;
static int reuse_overlap_observed = 0;
static int round_ready[EVENT_ROUNDS];
static int round_done[EVENT_ROUNDS];

// Purpose: read the number of threads blocked in event_wait().
// Inputs: none.
// Outputs: current waiter count.
// Preconditions: event initialized.
// Postconditions: none.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static unsigned event_waiter_count(void) {
  spin_lock_acquire(&event.cv.lock);
  unsigned waiters = event.cv.waiters;
  spin_lock_release(&event.cv.lock);
  return waiters;
}

// Purpose: block on the global event once and record phase progress.
// Inputs: arg points to EventWaitArgs for this waiter.
// Preconditions: event initialized; arg, ready, and done are non-NULL.
// Postconditions: increments *ready before waiting and *done after returning.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void initial_waiter_thread(void* arg) {
  struct EventWaitArgs* args = (struct EventWaitArgs*)arg;
  __atomic_fetch_add(args->ready, 1);
  event_wait(&event);
  __atomic_fetch_add(args->done, 1);
}

// Purpose: repeatedly wait on the same event across many generations.
// Inputs: arg points to the worker id.
// Preconditions: event initialized.
// Postconditions: increments round_ready[round] and round_done[round] once per round.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void reuse_waiter_thread(void* arg) {
  int id = *(int*)arg;

  __atomic_fetch_add(&reuse_started, 1);

  for (int round = 0; round < EVENT_ROUNDS; round++) {
    if (round > 0 && __atomic_load_n(&round_done[round - 1]) != REUSE_WAITERS) {
      __atomic_store_n(&reuse_overlap_observed, 1);
    }

    __atomic_fetch_add(&round_ready[round], 1);

    event_wait(&event);

    int seen = __atomic_load_n(&round_ready[round]);
    if (seen != REUSE_WAITERS) {
      int args[3] = { round, seen, REUSE_WAITERS };
      say("***event FAIL round=%d ready=%d expected=%d\n", args);
      panic("event test: stale signal leaked into the next generation\n");
    }

    if (id == SLOW_REUSE_WAITER_ID && round + 1 < EVENT_ROUNDS) {
      for (int i = 0; i < POST_SIGNAL_SLOW_YIELDS; i++) {
        yield();
      }
    }

    __atomic_fetch_add(&round_done[round], 1);
  }

  __atomic_fetch_add(&reuse_finished, 1);
}

// Purpose: allocate and start one initial waiter thread.
// Inputs: ready and done counters for the phase.
// Preconditions: event initialized; ready and done are non-NULL.
// Postconditions: one new waiter thread has been scheduled.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
static void spawn_initial_waiter(int* ready, int* done) {
  struct EventWaitArgs* args = malloc(sizeof(struct EventWaitArgs));
  assert(args != NULL, "event test: initial waiter arg allocation failed.\n");
  args->ready = ready;
  args->done = done;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "event test: Fun allocation failed.\n");
  fun->func = initial_waiter_thread;
  fun->arg = args;
  thread(fun);
}

// Purpose: allocate and start one reusable event waiter thread.
// Inputs: id is the worker id for this waiter.
// Preconditions: event initialized.
// Postconditions: one reusable waiter thread has been scheduled.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
static void spawn_reuse_waiter(int id) {
  int* arg = malloc(sizeof(int));
  assert(arg != NULL, "event test: reuse waiter id allocation failed.\n");
  *arg = id;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "event test: Fun allocation failed.\n");
  fun->func = reuse_waiter_thread;
  fun->arg = arg;
  thread(fun);
}

void kernel_main(void) {
  say("***event test start\n", NULL);

  event_init(&event);

  for (int i = 0; i < INITIAL_WAITERS; i++) {
    spawn_initial_waiter(&initial_ready, &initial_done);
  }

  for (int i = 0; i < READY_WAIT_BUDGET &&
                  __atomic_load_n(&initial_ready) != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&initial_ready) != INITIAL_WAITERS) {
    int args[2] = { __atomic_load_n(&initial_ready), INITIAL_WAITERS };
    say("***event FAIL initial ready=%d expected=%d\n", args);
    panic("event test: initial waiters did not start\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET &&
                  event_waiter_count() != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (event_waiter_count() != INITIAL_WAITERS) {
    int args[2] = { (int)event_waiter_count(), INITIAL_WAITERS };
    say("***event FAIL initial waiters=%d expected=%d\n", args);
    panic("event test: initial waiters did not block on the event\n");
  }

  event_signal(&event);

  for (int i = 0; i < DONE_WAIT_BUDGET &&
                  __atomic_load_n(&initial_done) != INITIAL_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&initial_done) != INITIAL_WAITERS) {
    int args[2] = { __atomic_load_n(&initial_done), INITIAL_WAITERS };
    say("***event FAIL initial done=%d expected=%d\n", args);
    panic("event test: first signal did not release all initial waiters\n");
  }

  for (int i = 0; i < WAITER_WAIT_BUDGET && event_waiter_count() != 0; i++) {
    yield();
  }
  if (event_waiter_count() != 0) {
    int args[2] = { (int)event_waiter_count(), 0 };
    say("***event FAIL waiters after first signal=%d expected=%d\n", args);
    panic("event test: initial waiters remained queued after signal\n");
  }

  for (int i = 0; i < REUSE_WAITERS; i++) {
    spawn_reuse_waiter(i);
  }

  for (int i = 0; i < READY_WAIT_BUDGET &&
                  __atomic_load_n(&reuse_started) != REUSE_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&reuse_started) != REUSE_WAITERS) {
    int args[2] = { __atomic_load_n(&reuse_started), REUSE_WAITERS };
    say("***event FAIL reuse started=%d expected=%d\n", args);
    panic("event test: reusable waiters did not start\n");
  }

  for (int round = 0; round < EVENT_ROUNDS; round++) {
    for (int i = 0; i < READY_WAIT_BUDGET &&
                    __atomic_load_n(&round_ready[round]) != REUSE_WAITERS;
         i++) {
      yield();
    }
    if (__atomic_load_n(&round_ready[round]) != REUSE_WAITERS) {
      int args[3] = { round, __atomic_load_n(&round_ready[round]), REUSE_WAITERS };
      say("***event FAIL round=%d ready=%d expected=%d\n", args);
      panic("event test: not all reusable waiters reached the round\n");
    }

    for (int i = 0; i < WAITER_WAIT_BUDGET &&
                    event_waiter_count() != REUSE_WAITERS;
         i++) {
      yield();
    }
    if (event_waiter_count() != REUSE_WAITERS) {
      int args[3] = { round, (int)event_waiter_count(), REUSE_WAITERS };
      say("***event FAIL round=%d waiters=%d expected=%d\n", args);
      panic("event test: a future waiter failed to block before the next signal\n");
    }

    event_signal(&event);

    for (int i = 0; i < DONE_WAIT_BUDGET &&
                    __atomic_load_n(&round_done[round]) != REUSE_WAITERS;
         i++) {
      yield();
    }
    if (__atomic_load_n(&round_done[round]) != REUSE_WAITERS) {
      int args[3] = { round, __atomic_load_n(&round_done[round]), REUSE_WAITERS };
      say("***event FAIL round=%d done=%d expected=%d\n", args);
      panic("event test: signal did not release all waiters for the round\n");
    }
  }

  for (int i = 0; i < DONE_WAIT_BUDGET &&
                  __atomic_load_n(&reuse_finished) != REUSE_WAITERS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&reuse_finished) != REUSE_WAITERS) {
    int args[2] = { __atomic_load_n(&reuse_finished), REUSE_WAITERS };
    say("***event FAIL reuse finished=%d expected=%d\n", args);
    panic("event test: reusable waiters did not finish all rounds\n");
  }

  if (__atomic_load_n(&reuse_overlap_observed) != 1) {
    say("***event FAIL non-sticky reuse overlap was not exercised\n", NULL);
    panic("event test: stale-signal coverage did not execute\n");
  }

  for (int round = 0; round < EVENT_ROUNDS; round++) {
    int ready_count = __atomic_load_n(&round_ready[round]);
    int done_count = __atomic_load_n(&round_done[round]);
    if (ready_count != REUSE_WAITERS || done_count != REUSE_WAITERS) {
      int args[4] = { round, ready_count, done_count, REUSE_WAITERS };
      say("***event FAIL round=%d ready=%d done=%d expected=%d\n", args);
      panic("event test: reusable round totals mismatch\n");
    }
  }

  event_destroy(&event);

  say("***event ok\n", NULL);
  say("***event test complete\n", NULL);
}
