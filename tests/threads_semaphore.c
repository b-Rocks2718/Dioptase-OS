/*
 * Semaphore test.
 *
 * Validates:
 * - sem_down blocks workers until main posts the semaphore
 * - one sem_up wakes one waiter and no worker runs more than once
 *
 * How:
 * - start NUM_WORKERS waiters on start_sem
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

static struct Semaphore start_sem;
static struct Semaphore done_sem;

static int ready = 0;
static int started = 0;
static int finished = 0;
static int progress[NUM_WORKERS];

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

  sem_init(&start_sem, 0);
  sem_init(&done_sem, 0);

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
