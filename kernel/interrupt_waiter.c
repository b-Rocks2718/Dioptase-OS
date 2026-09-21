#include "interrupt_waiter.h"

#include "debug.h"

// Initialize a waiter with no published thread or pending interrupt.
void interrupt_waiter_init(struct InterruptWaiter* waiter){
  assert(waiter != NULL,
    "interrupt waiter init: waiter pointer is NULL.\n");

  __atomic_store_n((int*)&waiter->thread, (int)NULL);
  __atomic_store_n(&waiter->event_pending, false);
}

// Clear stale interrupt state before a thread begins a new wait.
void interrupt_waiter_prepare(struct InterruptWaiter* waiter){
  assert(waiter != NULL,
    "interrupt waiter prepare: waiter pointer is NULL.\n");

  __atomic_store_n(&waiter->event_pending, false);
}

// Publish the waiting thread, returning it when an interrupt won the race.
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

// Record an interrupt and detach any thread that is already waiting.
struct TCB* interrupt_waiter_signal(struct InterruptWaiter* waiter){
  /*
   * Publish the event first. If no thread is visible yet, the later publish
   * consumes this flag. Reversing these operations would recreate the lost
   * wakeup window this helper exists to close.
   */
  __atomic_store_n(&waiter->event_pending, true);
  return (struct TCB*)__atomic_exchange_n((int*)&waiter->thread, (int)NULL);
}
