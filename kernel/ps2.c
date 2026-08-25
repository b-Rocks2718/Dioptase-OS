#include "ps2.h"
#include "ps2_test.h"
#include "blocking_queue.h"
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

/*
 * The bootstrap compiler requires a literal global-array bound. Keep this
 * named value synchronized with MAX_CORES * (KEYBUF_CAPACITY - 1); ps2_init()
 * checks that relationship before the driver publishes any queue state.
 */
#define PS2_EVENT_POOL_CAPACITY 252

/*
 * The ISR rings can hold KEYBUF_CAPACITY - 1 events per core because they
 * reserve one slot to distinguish full from empty. Give the worker-to-reader
 * handoff enough static elements to drain every ISR ring once without an
 * allocation. This is a capacity bound, not a promise that an indefinitely
 * slow reader can retain every future keyboard event.
 *
 * Ownership invariant, under the architecture's sequentially-consistent
 * memory model: each element is in exactly one place--ps2_free_queue,
 * ps2_queue, the sole worker's local variable, or one reader's local variable.
 * The worker is the only free-to-event producer. getkey()/waitkey() are the
 * event-to-free producers and may run concurrently on multiple cores. The two
 * BlockingQueues serialize those transfers and publish element->key before an
 * event becomes visible to a reader.
 */
static struct KeyElement ps2_event_pool[PS2_EVENT_POOL_CAPACITY];
static struct BlockingQueue ps2_free_queue;
static struct BlockingQueue ps2_queue;
static struct InterruptWaiter ps2_worker_waiter;
// Atomic builtins operate on the architecture's 32-bit int storage. The public
// accessor interprets this counter's bits as unsigned modulo-2^32 state.
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

// Record one nonzero event dropped at either bounded PS/2 queue boundary.
// Atomic fetch-add is a single bounded ISR-safe operation. The public 32-bit
// diagnostic count consequently wraps modulo 2^32 after UINT_MAX drops.
static void ps2_record_dropped_event(void){
  __atomic_fetch_add(&ps2_dropped_events, 1);
}

/*
 * Move one event from worker-local ownership into the shared event queue.
 *
 * Preconditions: kernel thread context, key != 0, and ps2_init() has completed.
 * This is never called by the ISR; the interrupt-side SPSC rings remain the
 * only ISR-owned queue. The caller must not retain key after this function.
 *
 * Postcondition: true means one pool element now owns key in ps2_queue. False
 * means no free element was immediately available, the event was discarded,
 * and the aggregate drop counter was incremented exactly once. The worker does
 * not block for a reader because that could let every per-core ISR ring fill
 * while the sole draining thread waited for pool storage.
 */
static bool ps2_publish_event(short key){
  assert(key != 0, "PS/2 publish: zero is not a keyboard event.\n");

  struct KeyElement* element =
    (struct KeyElement*)blocking_queue_try_remove(&ps2_free_queue);
  if (element == NULL){
    ps2_record_dropped_event();
    return false;
  }

  element->key = key;
  blocking_queue_add(&ps2_queue, &element->link);
  return true;
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
        ps2_publish_event(key);
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
  int required_capacity = MAX_CORES * (KEYBUF_CAPACITY - 1);
  if (PS2_EVENT_POOL_CAPACITY != required_capacity){
    int args[4] = {PS2_EVENT_POOL_CAPACITY, required_capacity,
      MAX_CORES, KEYBUF_CAPACITY};
    say("| PS/2 init rejected pool=%d required=%d max_cores=%d keybuf_slots=%d\n",
      args);
    panic("PS/2 init: static event-pool capacity does not match all usable ISR ring slots.\n");
  }

  blocking_queue_init(&ps2_queue);
  blocking_queue_init(&ps2_free_queue);

  /*
   * Initialization runs on core 0 in kernel mode after bootstrap() has made
   * CLH queue operations available, but before the PS/2 IVT entry is installed
   * or any secondary core is awake. Publish every detached static element to
   * the private free queue before either producer or consumer can run.
   */
  for (unsigned i = 0; i < PS2_EVENT_POOL_CAPACITY; ++i){
    ps2_event_pool[i].key = 0;
    blocking_queue_add(&ps2_free_queue, &ps2_event_pool[i].link);
  }

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

// Count and detach one queue-owned list without freeing its static elements.
static unsigned ps2_count_drained_elements(
    struct GenericQueueElement* elements){
  unsigned count = 0;
  while (elements != NULL){
    struct GenericQueueElement* next = elements->next;
    elements->next = NULL;
    elements = next;
    count++;
  }
  return count;
}

/*
 * Destroy the PS/2 synchronization state during globally quiescent shutdown.
 *
 * Preconditions: every core has disabled interrupts and entered the shutdown
 * barrier; the worker and all getkey()/waitkey() callers are stopped, so no
 * element can be locally owned and no queue operation can begin. The daemon
 * worker may remain published in ps2_worker_waiter, but its TCB and thread
 * storage are intentionally boot-lifetime allocations and it cannot execute.
 *
 * Postcondition: both BlockingQueues are empty before their locks/semaphores
 * are destroyed. All pool elements remain in static storage and none is passed
 * to free(). Exact accounting detects a shutdown that violated quiescence and
 * left an element transient in a worker or reader.
 */
void ps2_destroy(void){
  struct GenericQueueElement* queued =
    blocking_queue_remove_all(&ps2_queue);
  struct GenericQueueElement* free_elements =
    blocking_queue_remove_all(&ps2_free_queue);
  unsigned queued_count = ps2_count_drained_elements(queued);
  unsigned free_count = ps2_count_drained_elements(free_elements);
  unsigned expected = PS2_EVENT_POOL_CAPACITY;

  if (queued_count + free_count != expected){
    int args[3] = {(int)queued_count, (int)free_count, (int)expected};
    say("| PS/2 destroy rejected queued=%u free=%u expected=%u\n", args);
    panic("PS/2 destroy: event pool is transient or corrupt; stop and join every queue user before shutdown.\n");
  }

  blocking_queue_destroy(&ps2_queue);
  blocking_queue_destroy(&ps2_free_queue);
  interrupt_waiter_init(&ps2_worker_waiter);
  __atomic_store_n(&ps2_dropped_events, 0);
}

unsigned ps2_dropped_event_count(void){
  return (unsigned)__atomic_load_n(&ps2_dropped_events);
}

unsigned ps2_event_pool_capacity(void){
  return PS2_EVENT_POOL_CAPACITY;
}

bool ps2_test_publish_event(short key){
  assert(key != 0,
    "PS/2 test publish: zero cannot be injected as a keyboard event.\n");
  return ps2_publish_event(key);
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
  blocking_queue_add(&ps2_free_queue, &element->link);

  return key;
}

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the guest keycode event, clears the key from the buffer
short waitkey(void){
  struct KeyElement* element = (struct KeyElement*)blocking_queue_remove(&ps2_queue);
  short key = element->key;
  blocking_queue_add(&ps2_free_queue, &element->link);

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

  // Zero is the device's "no key" sentinel, not an event. Publishing it into
  // KeyBuf would be ambiguous with keybuf_remove()'s empty result: a later real
  // event could remain behind that sentinel after the worker consumed the one
  // wake notification. Acknowledge a spurious/empty interrupt without
  // mutating either bounded queue.
  if (key == 0){
    mark_ps2_handled();
    return;
  }

  /*
   * Capture the single hardware event before acknowledging the interrupt. The
   * fixed-size per-core SPSC buffer keeps this path allocation- and lock-free.
   */
  bool queued = keybuf_add(kb, key);
  mark_ps2_handled();

  if (!queued){
    ps2_record_dropped_event();
  }

  struct TCB* worker = interrupt_waiter_signal(&ps2_worker_waiter);
  if (worker != NULL){
    scheduler_wake_thread_from_interrupt(worker);
  }
}
