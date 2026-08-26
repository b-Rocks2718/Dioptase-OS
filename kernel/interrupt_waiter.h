#ifndef INTERRUPT_WAITER_H
#define INTERRUPT_WAITER_H

#include "TCB.h"

/*
 * Single-thread waiter handoff for asynchronous kernel producers, including
 * interrupt-driven devices.
 *
 * Concurrency contract:
 * - Exactly one thread may publish itself as the waiter.
 * - Device ISRs or kernel-thread producers may signal from any core.
 * - All atomic operations use the architecture's sequentially consistent
 *   memory model; no relaxed ordering is assumed.
 * - A non-NULL TCB returned by publish or signal is detached from the waiter
 *   and owned by the caller, which must enqueue exactly one scheduler wake
 *   through the path appropriate to its execution context.
 *
 * CPU-state contract:
 * - init runs in kernel mode before any producer is enabled, or during
 *   shutdown after no producer or consumer can execute.
 * - prepare runs in the sole worker/consumer thread in kernel mode. Interrupts
 *   may be enabled; cross-core exclusion comes from the atomic event state.
 * - publish runs in the idle context after block() has completely saved the
 *   outgoing thread. Current-core interrupts are disabled, and the published
 *   TCB is neither running nor present in another scheduler queue.
 * - signal runs in kernel mode. In ISR context, global interrupts are disabled
 *   by hardware and the caller must use an interrupt-safe scheduler wake. In
 *   thread context, the caller may use the ordinary scheduler wake path.
 *   Neither caller may directly context switch or modify the returned TCB.
 *
 * Neither operation takes a lock, allocates memory, loops, or changes the
 * interrupt mask, so signal is safe for bounded-time interrupt handlers.
 */
struct InterruptWaiter {
  struct TCB* thread;
  int event_pending;
};

// Postcondition: no thread is published and no device event is pending.
void interrupt_waiter_init(struct InterruptWaiter* waiter);

/*
 * Clear event state before the owning thread drains or directly checks the
 * backing queue/device. An event concurrent with this store is safe: it is
 * either represented in that backing state or remains pending for publish.
 * Postcondition: earlier notifications are consumed; a later notification is
 * preserved by signal or represented in the device state the caller checks.
 */
void interrupt_waiter_prepare(struct InterruptWaiter* waiter);

/*
 * Publish one already-blocked thread. If a producer signalled before the
 * publication became visible, atomically detach and return the thread so the
 * post-switch callback can defer its wake. Otherwise leave it published and
 * return NULL for a future signal to detach.
 * Postcondition: a non-NULL return is detached; a NULL return leaves thread
 * published unless a concurrent signal already detached it.
 */
struct TCB* interrupt_waiter_publish(struct InterruptWaiter* waiter,
  struct TCB* thread);

/*
 * Record a producer event before looking for the waiter. Return and detach the
 * published thread, or NULL when publication has not happened yet. Publishing
 * later will consume event_pending and return the thread to that caller.
 * Postcondition: event_pending is true unless a concurrent publisher consumed
 * it; a non-NULL return is detached and owned exclusively by the signaler.
 */
struct TCB* interrupt_waiter_signal(struct InterruptWaiter* waiter);

#endif // INTERRUPT_WAITER_H
