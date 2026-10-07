/*
 * PS/2 mouse bounded event-pool test.
 *
 * Validates:
 * - the worker-to-reader pool has one element for every usable per-core ISR
 *   ring slot and accepts exactly that many events without heap allocation
 * - publishing one more event drops only the new event and increments the
 *   aggregate diagnostic counter exactly once
 * - accepted events keep FIFO order and all 32 bits through getmouse();
 *   mouse words with a negative WHEEL byte are negative ints, so this catches
 *   any 16-bit truncation or sign mangling on the queue path
 * - getmouse() recycles every consumed element so the full pool is reusable
 *
 * How:
 * - publish through a narrow kernel-test hook that shares the worker's
 *   free-pool/event-queue path without needing emulated mouse timing
 * - fill to capacity, reject one extra event, drain in order, then repeat
 *   with a disjoint event pattern to prove recycling
 *
 * The automated fixture must not inject host mouse input while this test runs
 * (headless runs have no mouse; with EMU_VGA=yes keep the pointer still).
 */

#include "../kernel/mouse.h"
#include "../kernel/mouse_test.h"
#include "../kernel/config.h"
#include "../kernel/queue.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static void fail_uint(char* operation, unsigned got, unsigned expected){ /* Report a mouse queue mismatch with both values. */
  int args[2] = {(int)got, (int)expected};
  say("***mouse queue FAIL got=0x%X expected=0x%X\n", args);
  panic(operation);
}

static void expect_uint(unsigned got, unsigned expected, char* operation){ /* Panic with context when a mouse queue check fails. */
  if (got != expected){
    fail_uint(operation, got, expected);
  }
}

static void expect_bool(bool got, bool expected, char* operation){ /* Compare a publication result through the numeric reporter. */
  expect_uint((unsigned)got, (unsigned)expected, operation);
}

// First pass: VALID bit, WHEEL = -1, and the index spread across DX/DY.
static int first_pass_event(unsigned i){
  return (int)(0xFF000008u | (i << 8));
}

// Second pass: left button held, WHEEL = +1, disjoint from the first pass.
static int second_pass_event(unsigned i){
  return (int)(0x01000009u | (i << 8));
}

void kernel_main(void){ /* Exercise mouse event ordering, drops, and bounded capacity. */
  say("***mouse queue test start\n", NULL);

  while (getmouse() != 0){
    // Recycle any externally injected events before deterministic checks.
  }

  unsigned capacity = mouse_event_pool_capacity();
  unsigned drops_before = mouse_dropped_event_count();

  expect_uint(capacity, (unsigned)(MAX_CORES * (EVENTBUF_CAPACITY - 1)),
    "mouse queue test: pool does not cover every usable ISR ring slot.\n");

  for (unsigned i = 0; i < capacity; ++i){
    expect_bool(mouse_test_publish_event(first_pass_event(i)), true,
      "mouse queue test: pool rejected an event before reaching capacity.\n");
  }
  expect_bool(mouse_test_publish_event(first_pass_event(capacity)), false,
    "mouse queue test: pool accepted an event beyond fixed capacity.\n");
  expect_uint(mouse_dropped_event_count(), drops_before + 1,
    "mouse queue test: pool exhaustion did not count exactly one drop.\n");
  say("***mouse fixed pool exhaustion ok\n", NULL);

  for (unsigned i = 0; i < capacity; ++i){
    expect_uint((unsigned)getmouse(), (unsigned)first_pass_event(i),
      "mouse queue test: accepted events lost FIFO order or event bits.\n");
  }
  expect_uint((unsigned)getmouse(), 0,
    "mouse queue test: event queue was not empty after FIFO drain.\n");
  say("***mouse FIFO ok\n", NULL);

  for (unsigned i = 0; i < capacity; ++i){
    expect_bool(mouse_test_publish_event(second_pass_event(i)), true,
      "mouse queue test: consumed pool element was not recycled.\n");
  }
  expect_uint(mouse_dropped_event_count(), drops_before + 1,
    "mouse queue test: successful recycled publications changed drop count.\n");

  for (unsigned i = 0; i < capacity; ++i){
    expect_uint((unsigned)getmouse(), (unsigned)second_pass_event(i),
      "mouse queue test: recycled pool lost FIFO order or event bits.\n");
  }
  expect_uint((unsigned)getmouse(), 0,
    "mouse queue test: recycled event queue was not empty after drain.\n");
  say("***mouse recycling and drop count ok\n", NULL);

  say("***mouse queue test complete\n", NULL);
}
