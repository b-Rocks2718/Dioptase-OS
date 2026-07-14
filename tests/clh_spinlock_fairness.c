/*
 * CLH spinlock fairness comparison.
 *
 * Validates:
 * - CLH lock waiters that are already queued are served before the releasing
 *   owner can reacquire the lock.
 * - The existing test-and-set spin lock is a mutual exclusion primitive but
 *   does not provide that FIFO fairness guarantee.
 *
 * How:
 * - start one owner thread in kernel_main() and several worker threads pinned
 *   to the other emulator cores
 * - wait until all workers are ready, then let them contend while the owner
 *   still holds the lock
 * - for the CLH phase, each worker publishes its TCB so the owner can observe
 *   that the worker has linked itself into the CLH queue (`my_pred != NULL`)
 * - have the owner release and immediately reacquire several times
 * - print the observed acquisition order without the test-harness `***`
 *   prefix, then print one deterministic `***` pass line after validation
 *
 * This is a diagnostic/demo test. The normal spinlock's exact order is allowed
 * to vary because unfair locks make no ordering promise; the CLH phase panics
 * if owner 0 bypasses a queued waiter.
 */

#include "../kernel/atomic.h"
#include "../kernel/config.h"
#include "../kernel/constants.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/machine.h"
#include "../kernel/print.h"
#include "../kernel/threads.h"
#include "../kernel/interrupts.h"
#include "../kernel/per_core.h"

#define MAX_WAITERS 3
#define OWNER_ID 0
#define LOCK_KIND_NORMAL 0
#define LOCK_KIND_CLH 1
#define OWNER_REACQUIRE_ATTEMPTS 4
#define SETTLE_PAUSE_ITERS 20000
#define READY_WAIT_BUDGET 100000
#define DONE_WAIT_BUDGET 100000
#define QUEUE_WAIT_BUDGET 1000000
#define MAX_EVENTS 7
#define SEEN_SLOTS 4
#define SUPPORTED_AFFINITY_CORES 4

struct FairnessState {
  int ready;
  int attempting;
  int go;
  int done;
  int event_count;
  int order[MAX_EVENTS];
  struct TCB* worker_tcb[SEEN_SLOTS];
};

struct WorkerArg {
  int id;
  int lock_kind;
};

static struct SpinLock normal_lock;
static struct CLHLock clh_lock;

static struct FairnessState normal_state;
static struct FairnessState clh_state;
static int owner_core_id = 0;

static enum CoreAffinity core_affinities[SUPPORTED_AFFINITY_CORES] = {
  CORE_0,
  CORE_1,
  CORE_2,
  CORE_3
};

static void reset_state(struct FairnessState* state) {
  state->ready = 0;
  state->attempting = 0;
  state->go = 0;
  state->done = 0;
  state->event_count = 0;
  for (int i = 0; i < MAX_EVENTS; i++) {
    state->order[i] = -1;
  }
  for (int i = 0; i < SEEN_SLOTS; i++) {
    state->worker_tcb[i] = NULL;
  }
}

// Record one acquisition while the corresponding lock is held.
static void record_owner(struct FairnessState* state, int owner) {
  int index = state->event_count;
  if (index < MAX_EVENTS) {
    state->order[index] = owner;
  }
  state->event_count = index + 1;
}

static struct FairnessState* state_for_kind(int lock_kind) {
  if (lock_kind == LOCK_KIND_CLH) {
    return &clh_state;
  }
  return &normal_state;
}

static void fairness_worker(void* arg) {
  struct WorkerArg* worker = (struct WorkerArg*)arg;
  struct FairnessState* state = state_for_kind(worker->lock_kind);

  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();
  interrupts_restore(was);
  __atomic_store_n((int*)&state->worker_tcb[worker->id], (int)me);

  __atomic_fetch_add(&state->ready, 1);
  while (__atomic_load_n(&state->go) == 0) {
    pause();
  }
  __atomic_fetch_add(&state->attempting, 1);

  if (worker->lock_kind == LOCK_KIND_CLH) {
    clh_lock_acquire(&clh_lock);
    record_owner(state, worker->id);
    clh_lock_release(&clh_lock);
  } else {
    spin_lock_acquire(&normal_lock);
    record_owner(state, worker->id);
    spin_lock_release(&normal_lock);
  }

  __atomic_fetch_add(&state->done, 1);
}

static enum CoreAffinity worker_affinity(int worker_index) {
  int cores = CONFIG.num_cores;
  if (cores > SUPPORTED_AFFINITY_CORES) {
    cores = SUPPORTED_AFFINITY_CORES;
  }

  int core = (owner_core_id + worker_index + 1) % cores;
  return core_affinities[core];
}

static void spawn_worker(int lock_kind, int id) {
  struct WorkerArg* arg = malloc(sizeof(struct WorkerArg));
  assert(arg != NULL, "clh fairness test: WorkerArg allocation failed.\n");
  arg->id = id;
  arg->lock_kind = lock_kind;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "clh fairness test: Fun allocation failed.\n");
  fun->func = fairness_worker;
  fun->arg = arg;

  thread_(fun, HIGH_PRIORITY, worker_affinity(id - 1));
}

static void wait_for_count(int* value, int expected, int budget, char* panic_msg) {
  for (int i = 0; i < budget && __atomic_load_n(value) != expected; i++) {
    yield();
  }
  if (__atomic_load_n(value) != expected) {
    int args[2] = { __atomic_load_n(value), expected };
    say("***clh fairness FAIL got=%d expected=%d\n", args);
    panic(panic_msg);
  }
}

static void settle_waiters(void) {
  for (int i = 0; i < SETTLE_PAUSE_ITERS; i++) {
    pause();
  }
}

static void wait_for_attempts(struct FairnessState* state, int waiters, char* panic_msg) {
  for (int i = 0; i < QUEUE_WAIT_BUDGET && __atomic_load_n(&state->attempting) != waiters; i++) {
    pause();
  }
  if (__atomic_load_n(&state->attempting) != waiters) {
    int args[2] = { __atomic_load_n(&state->attempting), waiters };
    say("***clh fairness FAIL attempting=%d expected=%d\n", args);
    panic(panic_msg);
  }
}

static int clh_queued_count(int waiters) {
  int queued = 0;
  for (int i = 1; i <= waiters; i++) {
    struct TCB* worker_tcb =
      (struct TCB*)__atomic_load_n((int*)&clh_state.worker_tcb[i]);
    if (worker_tcb != NULL &&
        __atomic_load_n((int*)&worker_tcb->my_pred) != 0) {
      queued += 1;
    }
  }
  return queued;
}

static void wait_for_clh_queue(int waiters) {
  for (int i = 0; i < QUEUE_WAIT_BUDGET && clh_queued_count(waiters) != waiters; i++) {
    pause();
  }

  int queued = clh_queued_count(waiters);
  if (queued != waiters) {
    int args[2] = { queued, waiters };
    say("***clh fairness FAIL queued=%d expected=%d\n", args);
    panic("clh fairness test: CLH workers did not enqueue while owner held lock\n");
  }
}

static void run_normal_phase(int waiters) {
  reset_state(&normal_state);
  spin_lock_init(&normal_lock);

  for (int i = 1; i <= waiters; i++) {
    spawn_worker(LOCK_KIND_NORMAL, i);
  }

  wait_for_count(&normal_state.ready, waiters, READY_WAIT_BUDGET,
                 "clh fairness test: normal workers did not become ready\n");

  spin_lock_acquire(&normal_lock);
  __atomic_store_n(&normal_state.go, 1);
  wait_for_attempts(&normal_state, waiters,
                    "clh fairness test: normal workers did not attempt the lock\n");
  settle_waiters();

  for (int i = 0; i < OWNER_REACQUIRE_ATTEMPTS; i++) {
    spin_lock_release(&normal_lock);
    spin_lock_acquire(&normal_lock);
    record_owner(&normal_state, OWNER_ID);
  }

  spin_lock_release(&normal_lock);

  wait_for_count(&normal_state.done, waiters, DONE_WAIT_BUDGET,
                 "clh fairness test: normal workers did not finish\n");
}

static void run_clh_phase(int waiters) {
  reset_state(&clh_state);
  clh_lock_init(&clh_lock);

  for (int i = 1; i <= waiters; i++) {
    spawn_worker(LOCK_KIND_CLH, i);
  }

  wait_for_count(&clh_state.ready, waiters, READY_WAIT_BUDGET,
                 "clh fairness test: CLH workers did not become ready\n");

  clh_lock_acquire(&clh_lock);
  __atomic_store_n(&clh_state.go, 1);
  wait_for_clh_queue(waiters);
  settle_waiters();

  for (int i = 0; i < OWNER_REACQUIRE_ATTEMPTS; i++) {
    clh_lock_release(&clh_lock);
    clh_lock_acquire(&clh_lock);
    record_owner(&clh_state, OWNER_ID);
  }

  clh_lock_release(&clh_lock);

  wait_for_count(&clh_state.done, waiters, DONE_WAIT_BUDGET,
                 "clh fairness test: CLH workers did not finish\n");
  clh_lock_destroy(&clh_lock);
}

static int waiters_before_owner(struct FairnessState* state) {
  int count = 0;
  for (int i = 0; i < state->event_count && i < MAX_EVENTS; i++) {
    if (state->order[i] == OWNER_ID) {
      return count;
    }
    count += 1;
  }
  return count;
}

static int owner_barges_before_all_waiters(struct FairnessState* state, int waiters) {
  int seen[SEEN_SLOTS];
  int seen_waiters = 0;
  int barges = 0;

  for (int i = 0; i < SEEN_SLOTS; i++) {
    seen[i] = 0;
  }

  for (int i = 0; i < state->event_count && i < MAX_EVENTS; i++) {
    int owner = state->order[i];
    if (owner == OWNER_ID) {
      if (seen_waiters < waiters) {
        barges += 1;
      }
    } else if (owner > 0 && owner <= MAX_WAITERS && !seen[owner]) {
      seen[owner] = 1;
      seen_waiters += 1;
    }
  }

  return barges;
}

static void print_phase(int lock_kind, struct FairnessState* state, int waiters) {
  int args[2];
  args[0] = state->event_count;
  args[1] = waiters;
  if (lock_kind == LOCK_KIND_CLH) {
    say("clh events=%d waiters=%d\n", args);
  } else {
    say("normal events=%d waiters=%d\n", args);
  }

  for (int i = 0; i < state->event_count && i < MAX_EVENTS; i++) {
    int event_args[2] = { i, state->order[i] };
    if (lock_kind == LOCK_KIND_CLH) {
      say("clh order[%d]=%d\n", event_args);
    } else {
      say("normal order[%d]=%d\n", event_args);
    }
  }

  int summary[3] = {
    waiters_before_owner(state),
    waiters,
    owner_barges_before_all_waiters(state, waiters)
  };
  if (lock_kind == LOCK_KIND_CLH) {
    say("clh waiters-before-owner=%d/%d owner-barges=%d\n", summary);
  } else {
    say("normal waiters-before-owner=%d/%d owner-barges=%d\n", summary);
  }
}

void kernel_main(void) {
  say("clh fairness test start\n", NULL);

  if (CONFIG.num_cores < 2) {
    say("clh fairness test skipped: needs at least 2 cores\n", NULL);
    say("***clh fairness test pass\n", NULL);
    say("clh fairness test complete\n", NULL);
    return;
  }

  enum CoreAffinity prev_affinity = core_pin();
  owner_core_id = get_core_id();

  int waiters = CONFIG.num_cores - 1;
  if (waiters > MAX_WAITERS) {
    waiters = MAX_WAITERS;
  }

  run_normal_phase(waiters);
  run_clh_phase(waiters);

  print_phase(LOCK_KIND_NORMAL, &normal_state, waiters);
  print_phase(LOCK_KIND_CLH, &clh_state, waiters);

  int clh_barges = owner_barges_before_all_waiters(&clh_state, waiters);
  if (clh_barges != 0) {
    int args[1] = { clh_barges };
    say("***clh fairness FAIL owner-barges=%d\n", args);
    panic("clh fairness test: CLH owner bypassed a queued waiter\n");
  }

  core_unpin(prev_affinity);
  say("***clh fairness test pass\n", NULL);
  say("clh fairness test complete\n", NULL);
}
