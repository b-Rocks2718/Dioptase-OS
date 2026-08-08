#include "ps2.h"
#include "threads.h"
#include "machine.h"
#include "interrupts.h"
#include "print.h"
#include "debug.h"
#include "heap.h"
#include "per_core.h"
#include "scheduler.h"
#include "ivt.h"
#include "interrupt_waiter.h"

struct KeyElement {
  struct GenericQueueElement link;
  short key;
};

struct BlockingQueue ps2_queue;
static struct InterruptWaiter ps2_worker_waiter;
static int ps2_dropped_events;

// PS/2 MMIO address for keyboard input
static short* ps2_in = (short*)0x7FE5800;

static void ps2_worker_block(void* arg){
  struct TCB* tcb = (struct TCB*)arg;
  struct TCB* wakeup = interrupt_waiter_publish(&ps2_worker_waiter, tcb);

  if (wakeup != NULL){
    scheduler_wake_thread_from_interrupt(wakeup);
  }
}

// PS/2 worker thread to fill ps2_queue from bounded per-core ISR buffers.
static void ps2_worker(void){
  while (true){
    /*
     * Clear stale notification state before draining. Each key buffer is SPSC:
     * its core's ISR is the sole producer and this worker is the sole consumer.
     * An IRQ racing with this pass either leaves a buffered key to drain or
     * leaves event_pending set so the block callback immediately requeues us.
     */
    interrupt_waiter_prepare(&ps2_worker_waiter);

    for (int i = 0; i < MAX_CORES; ++i){
      short key = 0;
      while ((key = keybuf_remove(&per_core_data[i].keybuf)) != 0){
        struct KeyElement* element = malloc(sizeof(struct KeyElement));
        element->key = key;
        blocking_queue_add(&ps2_queue, (struct GenericQueueElement*)element);
      }
    }

    int was = interrupts_disable();
    struct TCB* me = get_current_tcb();
    block(was, ps2_worker_block, me, false);
  }
  panic("PS/2 worker thread exited unexpectedly");
}

// Initialize the PS/2 driver
void ps2_init(void){
  blocking_queue_init(&ps2_queue);

  for (int i = 0; i < MAX_CORES; ++i){
    keybuf_init(&per_core_data[i].keybuf);
  }

  interrupt_waiter_init(&ps2_worker_waiter);
  __atomic_store_n(&ps2_dropped_events, 0);

  // init ps2 worker thread
  struct Fun* ps2_worker_fun = leak(sizeof(struct Fun));
  ps2_worker_fun->func = (void (*)(void *))ps2_worker;
  ps2_worker_fun->arg = NULL;

  setup_thread(ps2_worker_fun, HIGH_PRIORITY, ANY_CORE);

  register_handler((void*)ps2_handler_, (void*)PS2_IVT_ENTRY);
}

// to be called only from kernel_shutdown
void ps2_destroy(void){
  blocking_queue_destroy(&ps2_queue);
  interrupt_waiter_init(&ps2_worker_waiter);
  __atomic_store_n(&ps2_dropped_events, 0);
}

unsigned ps2_dropped_event_count(void){
  return (unsigned)__atomic_load_n(&ps2_dropped_events);
}

// read a key from the PS/2 keyboard
// return the guest keycode event, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void){
  struct KeyElement* element = (struct KeyElement*)blocking_queue_try_remove(&ps2_queue);
  if (element == NULL) {
    return 0;
  }

  short key = element->key;

  free(element);

  return key;
}

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the guest keycode event, clears the key from the buffer
short waitkey(void){
  struct KeyElement* element = (struct KeyElement*)blocking_queue_remove(&ps2_queue);
  short key = element->key;

  free(element);

  return key;
}

// read a key from the PS/2 keyboard
// return the guest keycode event, or 0 if no key is pressed
// clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short getkey_raw(void){
  return *ps2_in;
}

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the guest keycode event, clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short waitkey_raw(void){
  short key = 0;
  while ((key = *ps2_in) == 0) {
    // spin until a key is pressed

    // sleep to save power
    pause();
  }
  return key;
}

void ps2_handler(void){
  struct PerCore* pc = get_per_core();
  struct KeyBuf* kb = &pc->keybuf;
  short key = *ps2_in;

  /*
   * Capture the single hardware event before acknowledging the interrupt. The
   * fixed-size per-core SPSC buffer keeps this path allocation- and lock-free.
   */
  bool queued = keybuf_add(kb, key);
  mark_ps2_handled();

  if (key == 0){
    return;
  }

  if (!queued){
    __atomic_fetch_add(&ps2_dropped_events, 1);
  }

  struct TCB* worker = interrupt_waiter_signal(&ps2_worker_waiter);
  if (worker != NULL){
    scheduler_wake_thread_from_interrupt(worker);
  }
}
