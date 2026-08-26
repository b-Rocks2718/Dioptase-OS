#ifndef PS2_TEST_H
#define PS2_TEST_H

#include "constants.h"

// Kernel-test interface for the bounded worker-to-reader handoff. Production
// code must receive keyboard events through the PS/2 interrupt path instead.

// Return the fixed event-pool capacity, which covers every usable per-core
// SPSC ring slot.
unsigned ps2_event_pool_capacity(void);

/*
 * Publish one event through the same free-pool/event-queue transfer as the
 * worker. Requires an initialized driver, a nonzero event, and no concurrent
 * host keyboard input. Returns false and records one drop when the pool is
 * full. The helper does not exercise interrupt waiter or SPSC-ring behavior.
 */
bool ps2_test_publish_event(short key);

#endif // PS2_TEST_H
