// Shared pointer parallel test.
// Purpose: validate shared pointer lifetime under concurrent cloning/dropping.

#include "../kernel/shared.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define WORKER_COUNT 8
#define ITERATIONS 128
#define PAYLOAD_MAGIC 0x5A5A

struct Payload {
  int value;
};

struct WorkerArg {
  struct StrongPtr* root;
  struct Barrier* start;
  struct Barrier* done;
  int id;
};

static int destroyed = 0;

// Purpose: payload destructor that tracks the number of destructions.
// Inputs: ptr points to a Payload allocated by the test.
// Preconditions: ptr is non-NULL.
// Postconditions: payload freed; destroyed incremented by one.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void payload_destructor(void* ptr) {
  struct Payload* payload = (struct Payload*)ptr;
  free(payload);
  __atomic_fetch_add(&destroyed, 1);
}

// Purpose: worker that clones a shared pointer and verifies deref stability.
// Inputs: arg points to a WorkerArg allocated by the test.
// Preconditions: arg is non-NULL; root/start/done are valid.
// Postconditions: local strong ref dropped; completion barrier reached.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void sharedptr_worker(void* arg) {
  struct WorkerArg* w = (struct WorkerArg*)arg;
  assert(w != NULL, "shared_ptr_parallel: WorkerArg is NULL.\n");

  struct StrongPtr local = strongptr_clone(w->root);
  assert(strongptr_not_null(&local),
         "shared_ptr_parallel: worker clone produced NULL StrongPtr.\n");

  // Synchronize so all workers hold a strong ref before the root drops.
  barrier_sync(w->start);

  for (int i = 0; i < ITERATIONS; ++i) {
    int value = ((struct Payload*)strongptr_deref(&local))->value;
    if (value != PAYLOAD_MAGIC) {
      int args[3] = { w->id, value, PAYLOAD_MAGIC };
      say("***shared_ptr_parallel FAIL id=%d value=%d expected=%d\n", args);
      panic("shared_ptr_parallel: payload value mismatch\n");
    }
    if ((i & 7) == 0) {
      yield();
    }
  }

  strongptr_drop(&local);
  barrier_sync(w->done);
}

void kernel_main(void) {
  say("***shared_ptr_parallel test start\n", NULL);

  __atomic_store_n(&destroyed, 0);

  struct Payload* payload = malloc(sizeof(struct Payload));
  assert(payload != NULL, "shared_ptr_parallel: payload allocation failed.\n");
  payload->value = PAYLOAD_MAGIC;

  struct StrongPtr root;
  strongptr_init(&root, payload, payload_destructor);
  assert(strongptr_not_null(&root),
         "shared_ptr_parallel: root StrongPtr is NULL after init.\n");

  struct Barrier start;
  struct Barrier done;
  barrier_init(&start, WORKER_COUNT + 1);
  barrier_init(&done, WORKER_COUNT + 1);

  for (int i = 0; i < WORKER_COUNT; ++i) {
    struct WorkerArg* arg = malloc(sizeof(struct WorkerArg));
    assert(arg != NULL, "shared_ptr_parallel: WorkerArg allocation failed.\n");
    arg->root = &root;
    arg->start = &start;
    arg->done = &done;
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "shared_ptr_parallel: Fun allocation failed.\n");
    fun->func = sharedptr_worker;
    fun->arg = arg;
    thread(fun);
  }

  barrier_sync(&start);
  strongptr_drop(&root);

  barrier_sync(&done);

  barrier_destroy(&start);
  barrier_destroy(&done);

  if (__atomic_load_n(&destroyed) != 1) {
    int args[2] = { __atomic_load_n(&destroyed), 1 };
    say("***shared_ptr_parallel FAIL destroyed=%d expected=%d\n", args);
    panic("shared_ptr_parallel: destructor count mismatch\n");
  }

  say("***shared_ptr_parallel ok\n", NULL);
  say("***shared_ptr_parallel test complete\n", NULL);
}
