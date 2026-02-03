// Semaphore test.
// Purpose: validate basic sem_down/sem_up behavior and wakeup ordering.

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

// Purpose: wait on start_sem, then signal completion through done_sem.
// Inputs: arg points to worker id.
// Preconditions: kernel mode; semaphores initialized.
// Postconditions: progress[id] increments once; done_sem signaled once.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void worker_thread(void* arg) {
  int id = *(int*)arg;

  __atomic_fetch_add(&ready, 1);

  sem_down(&start_sem);

  __atomic_fetch_add(&started, 1);
  progress[id] += 1;
  __atomic_fetch_add(&finished, 1);

  sem_up(&done_sem);
}

// Purpose: verify semaphore wakeups for multiple waiting threads.
// Inputs: none.
// Outputs: prints pass/fail and panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: all workers run exactly once after start signals.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***semaphore test start\n", NULL);

  sem_init(&start_sem, 0);
  sem_init(&done_sem, 0);

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

  // Release workers one by one and wait for completion each time.
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
