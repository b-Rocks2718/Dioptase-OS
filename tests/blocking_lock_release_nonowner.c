/*
 * Blocking-lock owner negative test.
 *
 * The owner is pinned to one core and retains the lock while kernel_main runs
 * on another. The lock's boolean held flag is true for both callers, so only
 * exact TCB identity can reject kernel_main's release attempt. This test needs
 * at least two configured cores and is filtered from one-core aggregates.
 */

#include "../kernel/blocking_lock.h"
#include "../kernel/threads.h"
#include "../kernel/per_core.h"
#include "../kernel/config.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

static struct BlockingLock lock;
static int owner_ready = 0;

static void owner_thread(void* unused){ /* Acquire the lock on a remote core and retain ownership for the negative release check. */
  (void)unused;
  blocking_lock_acquire(&lock);
  __atomic_store_n(&owner_ready, 1);
  while (true){
    pause();
  }
}

void kernel_main(void){ /* Verify releasing a blocking lock from a non-owner faults. */
  say("***blocking-lock nonowner-release negative start\n", NULL);
  assert(CONFIG.num_cores >= 2,
    "blocking-lock nonowner-release test: requires at least two cores.\n");
  blocking_lock_init(&lock);

  core_pin();
  int controller_core = get_core_id();
  int owner_core = controller_core == 0 ? 1 : 0;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL,
    "blocking-lock nonowner-release test: Fun allocation failed.\n");
  fun->func = owner_thread;
  fun->arg = NULL;
  thread_(fun, NORMAL_PRIORITY, (enum CoreAffinity)owner_core);

  while (__atomic_load_n(&owner_ready) == 0){
    yield();
  }

  blocking_lock_release(&lock);
  panic("blocking-lock nonowner-release test: release unexpectedly succeeded.\n");
}
