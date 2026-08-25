/*
 * Nonempty blocking-queue destruction negative test.
 *
 * Validates that composite queue teardown checks payload ownership before it
 * destroys semaphore/CLH state. The old behavior silently cleared the final
 * link to a caller-owned element. The corrected contract requires external
 * quiescence plus an explicit drain and emits a diagnostic panic here.
 */

#include "../kernel/blocking_queue.h"
#include "../kernel/print.h"

static struct BlockingQueue queue;
static struct GenericQueueElement element;

void kernel_main(void){
  say("***queue nonempty destroy negative start\n", NULL);
  blocking_queue_init(&queue);
  element.next = NULL;
  blocking_queue_add(&queue, &element);
  blocking_queue_destroy(&queue);
  say("***queue nonempty destroy negative FAIL\n", NULL);
}
