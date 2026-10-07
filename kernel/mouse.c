#include "mouse.h"
#include "mouse_test.h"
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

/*
 * PS/2 mouse driver. This mirrors the keyboard driver in ps2.c:
 *
 *   mouse IRQ (any core) -> that core's mouse_events SPSC ring
 *     -> one high-priority worker thread -> static event pool + BlockingQueue
 *     -> getmouse() readers
 *
 * The ISR copies exactly one MMIO event per interrupt. The emulator keeps the
 * mouse line routed to one core at a time until that core's `eoi 8`, and
 * re-raises it while the device queue is non-empty, so one-event-per-IRQ
 * drains the device without a loop in interrupt context. Each ISR stamps its
 * event with a global ticket before eoi, and the worker merges the per-core
 * rings by ticket, so readers see events in device order.
 */

// Carry one PS/2 mouse event word through the bounded worker queue.
struct MouseElement {
  struct GenericQueueElement link;
  int event;
};

/*
 * The bootstrap compiler requires a literal global-array bound. Keep this
 * named value synchronized with MAX_CORES * (EVENTBUF_CAPACITY - 1);
 * mouse_init() checks that relationship before the driver publishes any queue
 * state.
 */
#define MOUSE_EVENT_POOL_CAPACITY 252

/*
 * One pool element per usable ISR ring slot lets the worker drain every ring
 * once without allocating. If no reader consumes events (no program is using
 * the mouse), the pool fills and new events are dropped. Every event carries
 * the full button state, so the first event a reader sees after a drop
 * resynchronizes buttons; dropped relative motion is lost.
 *
 * Ownership invariant, under the architecture's sequentially-consistent
 * memory model: each element is in exactly one place--mouse_free_queue,
 * mouse_queue, the sole worker's local variable, or one reader's local
 * variable. The worker is the only free-to-event producer; getmouse() is the
 * event-to-free producer and may run concurrently on multiple cores. The two
 * BlockingQueues serialize those transfers and publish element->event before
 * an event becomes visible to a reader.
 */
static struct MouseElement mouse_event_pool[MOUSE_EVENT_POOL_CAPACITY];
static struct BlockingQueue mouse_free_queue;
static struct BlockingQueue mouse_queue;
static struct InterruptWaiter mouse_worker_waiter;
// Atomic builtins operate on the architecture's 32-bit int storage. The public
// accessor interprets this counter's bits as unsigned modulo-2^32 state.
static int mouse_dropped_events;
// Global ISR ticket counter. Consecutive interrupts land on different cores'
// rings; the worker merges rings by ticket to restore device order (see
// debugging/input_event_cross_core_reordering.md).
static int mouse_event_seq;
// Every core's mouse_events ring, for eventbuf_remove_oldest(). Set by mouse_init().
static struct EventBuf* mouse_rings[MAX_CORES];

// PS/2 mouse MMIO stream (docs/mem_map.md "PS/2 mouse input stream").
// A single aligned 32-bit load returns the oldest event and consumes it; byte
// and halfword loads are unspecified, so this must stay an int access.
static const volatile int * const mouse_in = (const volatile int *)0x7FE5808;

// Record one nonzero event dropped at either bounded mouse queue boundary.
// Atomic fetch-add is a single bounded ISR-safe operation.
static void mouse_record_dropped_event(void){
  __atomic_fetch_add(&mouse_dropped_events, 1);
}

/*
 * Move one event from worker-local ownership into the shared event queue.
 *
 * Preconditions: kernel thread context, event != 0, and mouse_init() has
 * completed. Never called by the ISR.
 *
 * Postcondition: true means one pool element now owns event in mouse_queue.
 * False means no free element was immediately available, the event was
 * discarded, and the drop counter was incremented exactly once. The worker
 * does not block for a reader because that could let every per-core ISR ring
 * fill while the sole draining thread waited for pool storage.
 */
static bool mouse_publish_event(int event){
  assert(event != 0, "mouse publish: zero is not a mouse event.\n");

  struct MouseElement* element =
    (struct MouseElement*)blocking_queue_try_remove(&mouse_free_queue);
  if (element == NULL){
    mouse_record_dropped_event();
    return false;
  }

  element->event = event;
  blocking_queue_add(&mouse_queue, &element->link);
  return true;
}

// Mouse worker thread: move events from the per-core ISR rings into
// mouse_queue. Kernel mode, high priority, any core; never exits.
static void mouse_worker(void){
  while (true){
    /*
     * Clear stale notification state before draining. Each ring is SPSC: its
     * core's ISR is the sole producer and this worker is the sole consumer.
     * An IRQ racing with this pass either leaves a buffered event to drain or
     * leaves event_pending set so the block callback immediately requeues us.
     */
    interrupt_waiter_prepare(&mouse_worker_waiter);

    // Drain all rings oldest-ticket first so events keep device order even
    // when consecutive interrupts were handled on different cores.
    int event = 0;
    while ((event = eventbuf_remove_oldest(mouse_rings, MAX_CORES)) != 0){
      mouse_publish_event(event);
    }

    interrupt_waiter_wait(&mouse_worker_waiter);
  }
  panic("mouse worker thread exited unexpectedly");
}

// Initialize the mouse driver (see mouse.h for the CPU-state precondition).
void mouse_init(void){
  int required_capacity = MAX_CORES * (EVENTBUF_CAPACITY - 1);
  if (MOUSE_EVENT_POOL_CAPACITY != required_capacity){
    int args[4] = {MOUSE_EVENT_POOL_CAPACITY, required_capacity,
      MAX_CORES, EVENTBUF_CAPACITY};
    say("| mouse init rejected pool=%d required=%d max_cores=%d ring_slots=%d\n",
      args);
    panic("mouse init: static event-pool capacity does not match all usable ISR ring slots.\n");
  }

  blocking_queue_init(&mouse_queue);
  blocking_queue_init(&mouse_free_queue);

  /*
   * Initialization runs on core 0 in kernel mode after bootstrap() has made
   * CLH queue operations available, but before the mouse IVT entry is
   * installed or any secondary core is awake. Publish every detached static
   * element to the private free queue before either producer or consumer can
   * run.
   */
  for (unsigned i = 0; i < MOUSE_EVENT_POOL_CAPACITY; ++i){
    mouse_event_pool[i].event = 0;
    blocking_queue_add(&mouse_free_queue, &mouse_event_pool[i].link);
  }

  for (int i = 0; i < MAX_CORES; ++i){
    eventbuf_init(&per_core_data[i].mouse_events);
    mouse_rings[i] = &per_core_data[i].mouse_events;
  }

  interrupt_waiter_init(&mouse_worker_waiter);
  __atomic_store_n(&mouse_dropped_events, 0);
  __atomic_store_n(&mouse_event_seq, 0);

  struct Fun* mouse_worker_fun = leak(sizeof(struct Fun));
  mouse_worker_fun->func = (void (*)(void *))mouse_worker;
  mouse_worker_fun->arg = NULL;

  setup_thread(mouse_worker_fun, HIGH_PRIORITY, ANY_CORE);

  // Install last: the handler may run as soon as a core enables
  // MOUSE_INT_ENABLE, and it relies on the rings and waiter above.
  register_handler((void*)mouse_handler_, (void*)MOUSE_IVT_ENTRY);
}

// Count and detach one queue-owned list without freeing its static elements.
static unsigned mouse_count_drained_elements(
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
 * Destroy the mouse synchronization state during globally quiescent shutdown.
 *
 * Preconditions: every core has disabled interrupts and entered the shutdown
 * barrier; the worker and all getmouse() callers are stopped, so no element
 * can be locally owned and no queue operation can begin. The daemon worker
 * may remain published in mouse_worker_waiter, but its TCB and thread storage
 * are boot-lifetime allocations and it cannot execute.
 *
 * Postcondition: both BlockingQueues are empty before their locks/semaphores
 * are destroyed. Exact accounting detects a shutdown that violated
 * quiescence and left an element transient in the worker or a reader.
 */
void mouse_destroy(void){
  struct GenericQueueElement* queued =
    blocking_queue_remove_all(&mouse_queue);
  struct GenericQueueElement* free_elements =
    blocking_queue_remove_all(&mouse_free_queue);
  unsigned queued_count = mouse_count_drained_elements(queued);
  unsigned free_count = mouse_count_drained_elements(free_elements);
  unsigned expected = MOUSE_EVENT_POOL_CAPACITY;

  if (queued_count + free_count != expected){
    int args[3] = {(int)queued_count, (int)free_count, (int)expected};
    say("| mouse destroy rejected queued=%u free=%u expected=%u\n", args);
    panic("mouse destroy: event pool is transient or corrupt; stop and join every queue user before shutdown.\n");
  }

  blocking_queue_destroy(&mouse_queue);
  blocking_queue_destroy(&mouse_free_queue);
  interrupt_waiter_init(&mouse_worker_waiter);
  __atomic_store_n(&mouse_dropped_events, 0);
}

// Return the number of mouse events dropped since mouse_init().
unsigned mouse_dropped_event_count(void){
  return (unsigned)__atomic_load_n(&mouse_dropped_events);
}

// Return the number of event nodes available in the bounded pool.
unsigned mouse_event_pool_capacity(void){
  return MOUSE_EVENT_POOL_CAPACITY;
}

// Publish a synthetic mouse event through the production worker path.
bool mouse_test_publish_event(int event){
  assert(event != 0,
    "mouse test publish: zero cannot be injected as a mouse event.\n");
  return mouse_publish_event(event);
}

// Return the oldest queued mouse event, or 0 if none is pending.
int getmouse(void){
  struct MouseElement* element =
    (struct MouseElement*)blocking_queue_try_remove(&mouse_queue);
  if (element == NULL){
    return 0;
  }

  int event = element->event;
  blocking_queue_add(&mouse_free_queue, &element->link);
  return event;
}

/*
 * Mouse interrupt handler, called from mouse_handler_.
 *
 * CPU state: kernel mode on whichever core the mouse line was routed to, with
 * global interrupts disabled by hardware for the whole call. Must not block,
 * allocate, or take locks; it touches only this core's SPSC ring (sole
 * producer) and the lock-free interrupt waiter.
 */
void mouse_handler(void){
  struct PerCore* pc = get_per_core();
  struct EventBuf* ring = &pc->mouse_events;
  int event = *mouse_in;

  // Zero is the device's "no event" sentinel, not an event. Storing it would
  // be ambiguous with eventbuf_remove()'s empty result. Acknowledge a
  // spurious/empty interrupt without mutating either bounded queue.
  if (event == 0){
    mark_mouse_handled();
    return;
  }

  /*
   * Capture the single hardware event before acknowledging the interrupt, so
   * the device cannot route the next interrupt for an event we have not yet
   * consumed from MMIO.
   */
  // Take the ticket before eoi: the device does not route the next mouse
  // interrupt (possibly to another core) until this core's eoi, so ticket
  // order matches MMIO order.
  unsigned seq = (unsigned)__atomic_fetch_add(&mouse_event_seq, 1);
  bool queued = eventbuf_add(ring, event, seq);
  mark_mouse_handled();

  if (!queued){
    mouse_record_dropped_event();
  }

  interrupt_waiter_notify_from_interrupt(&mouse_worker_waiter);
}
