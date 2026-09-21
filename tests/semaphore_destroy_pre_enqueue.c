/*
 * Semaphore pre-enqueue destruction negative test.
 *
 * Validates that sem_destroy() rejects an operation which has already entered
 * sem_down() and exchanged into the semaphore's CLH tail, even before the
 * post-switch callback can publish that TCB in wait_queue. The old destructor
 * could observe an empty wait queue, free a CLH node owned by a contender, and
 * later reap the blocked TCB without publishing its exit.
 *
 * Four pinned threads make the ordering deterministic. kernel_main pins itself
 * to its current controller core, then assigns the other three cores to the
 * lock holder, sem_down waiter, and destructor. The controller observes both
 * remote CLH tail exchanges before it releases the holder.
 *
 * The waiter releases its first count-check acquisition before its block()
 * callback runs. FIFO CLH order gives the destructor the lock next, while the
 * wait queue is still empty. active_operations must nevertheless be one, and
 * destruction must panic without freeing or reaping anything.
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/per_core.h"
#include "../kernel/machine.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/config.h"

// One controller plus three independently pinned CLH participants are needed
// to hold and observe both queued tail exchanges at the same time.
#define TEST_CORE_COUNT 4
#define TEST_WORKER_CORE_COUNT 3

static struct Semaphore target;

static int holder_start = 0;
static int holder_has_lock = 0;
static int holder_release = 0;
static int waiter_start = 0;
static int destroyer_start = 0;

static struct TCB* waiter_tcb = NULL;
static struct TCB* destroyer_tcb = NULL;

static void holder_thread(void* unused) { /* Hold the semaphore's CLH lock until both competing operations have queued. */
  (void)unused;
  while (__atomic_load_n(&holder_start) == 0) {
    yield();
  }

  // CLH acquisition disables interrupts. This bounded spin is safe because
  // the separately pinned controller publishes holder_release after observing
  // both remote tail exchanges.
  clh_lock_acquire(&target.lock);
  __atomic_store_n(&holder_has_lock, 1);
  while (__atomic_load_n(&holder_release) == 0) {
  }
  clh_lock_release(&target.lock);
}

static void waiter_thread(void* unused) { /* Enter sem_down far enough to publish an active operation and queue for the lock. */
  (void)unused;
  __atomic_store_n((int*)&waiter_tcb, (int)get_current_tcb());
  while (__atomic_load_n(&waiter_start) == 0) {
    yield();
  }
  sem_down(&target);
  panic("semaphore destroy pre-enqueue test: waiter unexpectedly resumed\n");
}

static void destroyer_thread(void* unused) { /* Attempt destruction while the waiter is active but not yet on the wait queue. */
  (void)unused;
  __atomic_store_n((int*)&destroyer_tcb, (int)get_current_tcb());
  while (__atomic_load_n(&destroyer_start) == 0) {
    yield();
  }
  sem_destroy(&target);
  panic("semaphore destroy pre-enqueue test: busy destruction unexpectedly succeeded\n");
}

static void start_pinned(void (*func)(void*), enum CoreAffinity core) { /* Start a worker with an explicit core affinity. */
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL,
    "semaphore destroy pre-enqueue test: Fun allocation failed.\n");
  fun->func = func;
  fun->arg = NULL;
  thread_(fun, NORMAL_PRIORITY, core);
}

void kernel_main(void) { /* Verify semaphore destruction rejects a pre-enqueue waiter race. */
  say("***semaphore destroy pre-enqueue test start\n", NULL);
  sem_init(&target, 0);

  // kernel_main is initially ANY_CORE, so do not assume it happens to run on
  // core 0. Pin it where it is and allocate each remaining hardware core to
  // one permanently concurrent role.
  assert(CONFIG.num_cores == TEST_CORE_COUNT,
    "semaphore destroy pre-enqueue test: test requires four configured cores.\n");
  core_pin();
  int controller_core = get_core_id();
  enum CoreAffinity worker_cores[TEST_WORKER_CORE_COUNT];
  int worker_core_count = 0;
  for (int core = 0; core < TEST_CORE_COUNT; core++) {
    if (core != controller_core) {
      worker_cores[worker_core_count++] = (enum CoreAffinity)core;
    }
  }
  assert(worker_core_count == TEST_WORKER_CORE_COUNT,
    "semaphore destroy pre-enqueue test: test requires exactly four cores.\n");

  start_pinned(holder_thread, worker_cores[0]);
  start_pinned(waiter_thread, worker_cores[1]);
  start_pinned(destroyer_thread, worker_cores[2]);

  while (__atomic_load_n((int*)&waiter_tcb) == 0 ||
      __atomic_load_n((int*)&destroyer_tcb) == 0) {
    yield();
  }

  __atomic_store_n(&holder_start, 1);
  while (__atomic_load_n(&holder_has_lock) == 0) {
    yield();
  }

  __atomic_store_n(&waiter_start, 1);
  while (__atomic_load_n(&target.active_operations) != 1) {
    yield();
  }
  while (__atomic_load_n((int*)&target.lock.tail) !=
      (int)waiter_tcb->my_node) {
    yield();
  }

  __atomic_store_n(&destroyer_start, 1);
  while (__atomic_load_n((int*)&target.lock.tail) !=
      (int)destroyer_tcb->my_node) {
    yield();
  }

  __atomic_store_n(&holder_release, 1);

  while (true) {
    yield();
  }
}
