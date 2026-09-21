/*
 * Bounded-buffer live-waiter destruction negative test.
 *
 * A blocked consumer leaves the payload queue empty and both visible permit
 * counts at their ordinary empty-buffer values. Those values alone therefore
 * cannot prove teardown is safe. The composite destructor must preflight both
 * embedded semaphores before destroying either one and reject the consumer's
 * still-live remove operation.
 */

#include "../kernel/bounded_buffer.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

#define TEST_BUFFER_CAPACITY 2

static struct BoundedBuffer buffer;

static void blocked_consumer(void* unused){ /* Wait for the buffer operation that should be released by destruction. */
  (void)unused;
  bounded_buffer_remove(&buffer);
  panic("bounded-buffer destroy waiter test: consumer unexpectedly resumed.\n");
}

void kernel_main(void){ /* Verify destroying a bounded buffer with a waiter is rejected. */
  say("***bounded-buffer destroy waiter negative start\n", NULL);
  bounded_buffer_init(&buffer, TEST_BUFFER_CAPACITY);

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL,
    "bounded-buffer destroy waiter test: Fun allocation failed.\n");
  fun->func = blocked_consumer;
  fun->arg = NULL;
  thread(fun);

  // Wait until sem_down() has both published the waiter and retained its
  // operation reference across the suspended continuation. At this point the
  // payload queue remains empty, which is the exact state that defeated the
  // old count-only composite preflight.
  while (__atomic_load_n(&buffer.remove_sem.active_operations) != 1 ||
      __atomic_load_n(&buffer.remove_sem.wait_queue.size) != 1){
    yield();
  }

  bounded_buffer_destroy(&buffer);
  panic("bounded-buffer destroy waiter test: busy destruction unexpectedly succeeded.\n");
}
