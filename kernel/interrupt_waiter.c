#include "interrupt_waiter.h"

#include "debug.h"
#include "interrupts.h"
#include "per_core.h"
#include "scheduler.h"
#include "threads.h"

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

// Arguments carried from interrupt_waiter_wait() into its block() continuation.
struct InterruptWaiterBlock {
  struct InterruptWaiter* waiter;
  struct TCB* thread;
};

// block() continuation: publish the now fully saved waiter. If a signal won
// the race, publish detaches the thread here and this callback owns its wake.
// Runs in the idle context with current-core interrupts disabled, so it uses
// the interrupt-safe wake path.
static void interrupt_waiter_block(void* arg){
  struct InterruptWaiterBlock* args = (struct InterruptWaiterBlock*)arg;
  struct TCB* wakeup = interrupt_waiter_publish(args->waiter, args->thread);
  if (wakeup != NULL){
    scheduler_wake_thread_from_interrupt(wakeup);
  }
}

// Block until the next producer signal; see the header for the protocol.
void interrupt_waiter_wait(struct InterruptWaiter* waiter){
  unsigned was = interrupts_disable();
  struct InterruptWaiterBlock args;
  args.waiter = waiter;
  args.thread = get_current_tcb();
  block(was, interrupt_waiter_block, &args, false);
}

// Signal from thread context and wake any detached waiter.
void interrupt_waiter_notify(struct InterruptWaiter* waiter){
  struct TCB* wakeup = interrupt_waiter_signal(waiter);
  if (wakeup != NULL){
    scheduler_wake_thread(wakeup);
  }
}

// Signal from an ISR and defer any detached waiter's wake.
void interrupt_waiter_notify_from_interrupt(struct InterruptWaiter* waiter){
  struct TCB* wakeup = interrupt_waiter_signal(waiter);
  if (wakeup != NULL){
    scheduler_wake_thread_from_interrupt(wakeup);
  }
}
