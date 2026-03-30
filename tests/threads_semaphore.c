/*
 * Semaphore test.
 *
 * Validates:
 * - sem_try_down consumes a permit when one is available and reports failure
 *   without blocking when the count is zero, even when several threads race
 *   for a fixed permit pool
 * - sem_down blocks workers until main posts the semaphore
 * - one sem_up wakes one waiter and no worker runs more than once
 *
 * How:
 * - first exercise sem_try_down on a small local semaphore fixture
 * - then start a contended sem_try_down race where many workers spin on one
 *   start flag and attempt to consume a small fixed number of permits
 * - then start NUM_WORKERS waiters on start_sem
 * - release them one at a time with sem_up()
 * - use done_sem plus per-thread progress counters to prove each worker runs
 *   exactly once
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/heap.h"

#define NUM_WORKERS 8
#define NUM_TRY_WORKERS 8
#define NUM_TRY_PERMITS 3

static struct Semaphore start_sem;
static struct Semaphore done_sem;
static struct Semaphore try_sem;

static int ready = 0;
static int started = 0;
static int finished = 0;
static int progress[NUM_WORKERS];
static int try_ready = 0;
static int try_done = 0;
static int try_success = 0;
static int try_failure = 0;
static bool try_go = false;

// Wait for the start flag, then race once on sem_try_down().
static void try_worker_thread(void* arg) {
  int id = *(int*)arg;

  __atomic_fetch_add(&try_ready, 1);

  while (!__atomic_load_n(&try_go)) {
    yield();
  }

  if (sem_try_down(&try_sem)) {
    progress[id] = 1;
    __atomic_fetch_add(&try_success, 1);
  } else {
    progress[id] = 0;
    __atomic_fetch_add(&try_failure, 1);
  }

  __atomic_fetch_add(&try_done, 1);
}

// Wait on start_sem, then record one unit of progress for this worker.
static void worker_thread(void* arg) {
  int id = *(int*)arg;

  __atomic_fetch_add(&ready, 1);

  // Stay blocked until main explicitly releases this worker.
  sem_down(&start_sem);

  __atomic_fetch_add(&started, 1);
  progress[id] += 1;
  __atomic_fetch_add(&finished, 1);

  sem_up(&done_sem);
}

// Release the waiter set one by one and verify each runs exactly once.
void kernel_main(void) {
  say("***semaphore test start\n", NULL);

  struct Semaphore local_sem;
  sem_init(&local_sem, 0);
  if (sem_try_down(&local_sem)) {
    panic("semaphore test: sem_try_down succeeded on an empty semaphore\n");
  }
  sem_up(&local_sem);
  if (!sem_try_down(&local_sem)) {
    panic("semaphore test: sem_try_down failed after sem_up\n");
  }
  if (sem_try_down(&local_sem)) {
    panic("semaphore test: sem_try_down consumed a non-existent second permit\n");
  }

  sem_init(&try_sem, NUM_TRY_PERMITS);
  try_ready = 0;
  try_done = 0;
  try_success = 0;
  try_failure = 0;
  try_go = false;
  for (int i = 0; i < NUM_TRY_WORKERS; i++) {
    progress[i] = -1;
  }

  for (int i = 0; i < NUM_TRY_WORKERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "semaphore test: try id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "semaphore test: try Fun allocation failed.\n");
    fun->func = try_worker_thread;
    fun->arg = id;

    thread(fun);
  }

  while (__atomic_load_n(&try_ready) != NUM_TRY_WORKERS) {
    yield();
  }

  try_go = true;

  while (__atomic_load_n(&try_done) != NUM_TRY_WORKERS) {
    yield();
  }

  if (__atomic_load_n(&try_success) != NUM_TRY_PERMITS) {
    int args[2] = { __atomic_load_n(&try_success), NUM_TRY_PERMITS };
    say("***semaphore FAIL try_success=%d expected=%d\n", args);
    panic("semaphore test: concurrent sem_try_down success count mismatch\n");
  }
  if (__atomic_load_n(&try_failure) != NUM_TRY_WORKERS - NUM_TRY_PERMITS) {
    int args[2] = { __atomic_load_n(&try_failure), NUM_TRY_WORKERS - NUM_TRY_PERMITS };
    say("***semaphore FAIL try_failure=%d expected=%d\n", args);
    panic("semaphore test: concurrent sem_try_down failure count mismatch\n");
  }
  for (int i = 0; i < NUM_TRY_WORKERS; i++) {
    if (progress[i] != 0 && progress[i] != 1) {
      int args[3] = { i, progress[i], 0 };
      say("***semaphore FAIL id=%d got=%d expected=%d/1\n", args);
      panic("semaphore test: sem_try_down worker did not record a result\n");
    }
  }
  if (sem_try_down(&try_sem)) {
    panic("semaphore test: concurrent sem_try_down left an unexpected permit\n");
  }

  sem_init(&start_sem, 0);
  sem_init(&done_sem, 0);
  ready = 0;
  started = 0;
  finished = 0;
  for (int i = 0; i < NUM_WORKERS; i++) {
    progress[i] = 0;
  }

  // Start the full waiter set on an empty semaphore.
  for (int i = 0; i < NUM_WORKERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "semaphore test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "semaphore test: Fun allocation failed.\n");
    fun->func = worker_thread;
    fun->arg = id;

    thread(fun);
  }

  while (__atomic_load_n(&ready) != NUM_WORKERS) {
    yield();
  }

  // Release workers one by one and wait for each completion signal.
  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_up(&start_sem);
    sem_down(&done_sem);
  }

  int total = __atomic_load_n(&finished);
  if (total != NUM_WORKERS) {
    int args[2] = { total, NUM_WORKERS };
    say("***semaphore FAIL finished=%d expected=%d\n", args);
    panic("semaphore test: finished count mismatch\n");
  }

  for (int i = 0; i < NUM_WORKERS; i++) {
    if (progress[i] != 1) {
      int args[3] = { i, progress[i], 1 };
      say("***semaphore FAIL id=%d got=%d expected=%d\n", args);
      panic("semaphore test: per-thread progress mismatch\n");
    }
  }

  say("***semaphore ok\n", NULL);
  say("***semaphore test complete\n", NULL);
}
