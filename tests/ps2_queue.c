/*
 * PS/2 bounded event-pool test.
 *
 * Validates:
 * - the worker-to-reader pool has one element for every usable per-core ISR
 *   ring slot and accepts exactly that many events without heap allocation
 * - publishing one more event drops only the new event and increments the
 *   aggregate diagnostic counter exactly once
 * - accepted events retain FIFO order through getkey()
 * - getkey() recycles every consumed element so the complete pool can be used
 *   again without reducing capacity or changing the drop counter
 *
 * How:
 * - run through a narrow kernel-test publication hook that shares the worker's
 *   actual free-pool/event-queue path but does not require emulated key timing
 * - fill to the reported capacity, reject one extra event, and drain in order
 * - repeat a full fill/drain with a disjoint key range to prove recycling
 *
 * The automated fixture must not inject host keyboard input while this test is
 * running. The interrupt-side SPSC rings have separate coverage in queue_test.
 */

#include "../kernel/ps2.h"
#include "../kernel/ps2_test.h"
#include "../kernel/config.h"
#include "../kernel/queue.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

// The hardware contract sets bit 8 on release events while preserving the
// corresponding nonzero low-byte guest keycode.
#define RELEASE_EVENT_FLAG 0x100

static void fail_uint(char* operation, unsigned got, unsigned expected){ /* Report a queue assertion with both observed and expected values. */
  int args[2] = {(int)got, (int)expected};
  say("***ps2 queue FAIL got=%u expected=%u\n", args);
  panic(operation);
}

static void expect_uint(unsigned got, unsigned expected, char* operation){ /* Report an unsigned PS/2 queue mismatch with its operation context. */
  if (got != expected){
    fail_uint(operation, got, expected);
  }
}

static void expect_bool(bool got, bool expected, char* operation){ /* Compare a PS/2 publication result through the shared numeric reporter. */
  expect_uint((unsigned)got, (unsigned)expected, operation);
}

static void drain_preexisting_events(void){ /* Recycle any externally injected key events before deterministic checks begin. */
  while (getkey() != 0){
    // Return any externally injected event element before deterministic checks.
  }
}

void kernel_main(void){ /* Exercise PS/2 event ordering, drops, and bounded capacity. */
  say("***ps2 queue test start\n", NULL);

  drain_preexisting_events();

  unsigned capacity = ps2_event_pool_capacity();
  unsigned expected_capacity =
    (unsigned)(MAX_CORES * (KEYBUF_CAPACITY - 1));
  unsigned drops_before = ps2_dropped_event_count();

  expect_uint(capacity, expected_capacity,
    "PS/2 queue test: pool does not cover every usable ISR ring slot.\n");

  for (unsigned i = 0; i < capacity; ++i){
    expect_bool(ps2_test_publish_event((short)(i + 1)), true,
      "PS/2 queue test: pool rejected an event before reaching capacity.\n");
  }
  expect_bool(ps2_test_publish_event((short)(capacity + 1)), false,
    "PS/2 queue test: pool accepted an event beyond fixed capacity.\n");
  expect_uint(ps2_dropped_event_count(), drops_before + 1,
    "PS/2 queue test: pool exhaustion did not count exactly one drop.\n");
  say("***ps2 fixed pool exhaustion ok\n", NULL);

  for (unsigned i = 0; i < capacity; ++i){
    expect_uint((unsigned)getkey(), i + 1,
      "PS/2 queue test: accepted events lost FIFO order.\n");
  }
  expect_uint((unsigned)getkey(), 0,
    "PS/2 queue test: event queue was not empty after FIFO drain.\n");
  say("***ps2 FIFO ok\n", NULL);

  for (unsigned i = 0; i < capacity; ++i){
    short release_event = (short)(RELEASE_EVENT_FLAG | (i + 1));
    expect_bool(ps2_test_publish_event(release_event), true,
      "PS/2 queue test: consumed pool element was not recycled.\n");
  }
  expect_uint(ps2_dropped_event_count(), drops_before + 1,
    "PS/2 queue test: successful recycled publications changed drop count.\n");

  for (unsigned i = 0; i < capacity; ++i){
    unsigned expected_event = RELEASE_EVENT_FLAG | (i + 1);
    expect_uint((unsigned)getkey(), expected_event,
      "PS/2 queue test: recycled pool lost FIFO order.\n");
  }
  expect_uint((unsigned)getkey(), 0,
    "PS/2 queue test: recycled event queue was not empty after drain.\n");
  say("***ps2 recycling and drop count ok\n", NULL);

  say("***ps2 queue test complete\n", NULL);
}
