#ifndef MOUSE_TEST_H
#define MOUSE_TEST_H

#include "constants.h"

// Kernel-test interface for the bounded worker-to-reader handoff. Production
// code must receive mouse events through the mouse interrupt path instead.

// Return the fixed event-pool capacity, which covers every usable per-core
// SPSC ring slot.
unsigned mouse_event_pool_capacity(void);

/*
 * Publish one event through the same free-pool/event-queue transfer as the
 * worker. Requires an initialized driver, a nonzero event, and no concurrent
 * host mouse input. Returns false and records one drop when the pool is full.
 * The helper does not exercise interrupt waiter or SPSC-ring behavior.
 */
bool mouse_test_publish_event(int event);

#endif // MOUSE_TEST_H
