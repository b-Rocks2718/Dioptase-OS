#include "interrupt_waiter.h"

#include "debug.h"

void interrupt_waiter_init(struct InterruptWaiter* waiter){
  assert(waiter != NULL,
    "interrupt waiter init: waiter pointer is NULL.\n");

  __atomic_store_n((int*)&waiter->thread, (int)NULL);
  __atomic_store_n(&waiter->event_pending, false);
}

void interrupt_waiter_prepare(struct InterruptWaiter* waiter){
  assert(waiter != NULL,
    "interrupt waiter prepare: waiter pointer is NULL.\n");

  __atomic_store_n(&waiter->event_pending, false);
}

struct TCB* interrupt_waiter_publish(struct InterruptWaiter* waiter,
    struct TCB* thread){
  assert(waiter != NULL,
    "interrupt waiter publish: waiter pointer is NULL.\n");
  assert(thread != NULL,
    "interrupt waiter publish: thread pointer is NULL.\n");
  assert(__atomic_load_n((int*)&waiter->thread) == (int)NULL,
    "interrupt waiter publish: another thread is already waiting.\n");

  __atomic_store_n((int*)&waiter->thread, (int)thread);

  if (!__atomic_exchange_n(&waiter->event_pending, false)){
    return NULL;
  }

  /*
   * A concurrent ISR may detach the thread after setting event_pending but
   * before this exchange. Atomic exchange assigns the one wake to exactly one
   * caller: the other caller observes NULL and must do nothing.
   */
  return (struct TCB*)__atomic_exchange_n((int*)&waiter->thread, (int)NULL);
}

struct TCB* interrupt_waiter_signal(struct InterruptWaiter* waiter){
  /*
   * Publish the event first. If no thread is visible yet, the later publish
   * consumes this flag. Reversing these operations would recreate the lost
   * wakeup window this helper exists to close.
   */
  __atomic_store_n(&waiter->event_pending, true);
  return (struct TCB*)__atomic_exchange_n((int*)&waiter->thread, (int)NULL);
}
