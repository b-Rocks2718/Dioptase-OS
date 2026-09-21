/*
 * Interrupt waiter handoff test.
 *
 * Validates:
 * - an interrupt immediately before waiter publication is consumed by the
 *   post-switch publisher
 * - an interrupt immediately after publication detaches the waiter itself
 * - exactly one side owns each wake and stale events can be cleared before a
 *   driver drains or checks its device
 * - repeated pre-publication interrupts coalesce without losing the wake
 *
 * How:
 * - drive the helper's atomic operations in each possible linearized order
 * - use one static TCB as the single waiter and verify which operation returns
 *   ownership of it
 */

#include "../kernel/interrupt_waiter.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static struct TCB waiter_thread;

static void expect_thread(char* message, struct TCB* got, /* Require the handoff operation to return the expected waiter. */
    struct TCB* expected){
  if (got != expected){
    int args[2] = { (int)got, (int)expected };
    say("***interrupt waiter FAIL got=0x%X expected=0x%X\n", args);
    panic(message);
  }
}

static void test_signal_before_publish(void){ /* Verify publication consumes a signal that arrived before a waiter was visible. */
  struct InterruptWaiter waiter;

  interrupt_waiter_init(&waiter);
  interrupt_waiter_prepare(&waiter);

  expect_thread(
    "interrupt waiter test: pre-publication signal unexpectedly found a thread.\n",
    interrupt_waiter_signal(&waiter), NULL);
  expect_thread(
    "interrupt waiter test: publisher did not consume the pending signal.\n",
    interrupt_waiter_publish(&waiter, &waiter_thread), &waiter_thread);
}

static void test_publish_before_signal(void){ /* Verify a later signal detaches an already-published waiter. */
  struct InterruptWaiter waiter;

  interrupt_waiter_init(&waiter);
  interrupt_waiter_prepare(&waiter);

  expect_thread(
    "interrupt waiter test: publisher woke without a pending signal.\n",
    interrupt_waiter_publish(&waiter, &waiter_thread), NULL);
  expect_thread(
    "interrupt waiter test: post-publication signal did not detach the thread.\n",
    interrupt_waiter_signal(&waiter), &waiter_thread);
}

static void test_prepare_clears_stale_signal(void){ /* Verify preparing a new wait discards signal state from the preceding cycle. */
  struct InterruptWaiter waiter;

  interrupt_waiter_init(&waiter);
  expect_thread(
    "interrupt waiter test: stale signal unexpectedly found a thread.\n",
    interrupt_waiter_signal(&waiter), NULL);

  interrupt_waiter_prepare(&waiter);
  expect_thread(
    "interrupt waiter test: prepare did not clear stale signal state.\n",
    interrupt_waiter_publish(&waiter, &waiter_thread), NULL);
  expect_thread(
    "interrupt waiter test: signal after prepare did not detach the thread.\n",
    interrupt_waiter_signal(&waiter), &waiter_thread);
}

static void test_prepublication_signals_coalesce(void){ /* Verify repeated early signals produce exactly one wake on publication. */
  struct InterruptWaiter waiter;

  interrupt_waiter_init(&waiter);
  interrupt_waiter_prepare(&waiter);

  expect_thread(
    "interrupt waiter test: first coalesced signal found a thread.\n",
    interrupt_waiter_signal(&waiter), NULL);
  expect_thread(
    "interrupt waiter test: second coalesced signal found a thread.\n",
    interrupt_waiter_signal(&waiter), NULL);
  expect_thread(
    "interrupt waiter test: coalesced signals did not produce one wake.\n",
    interrupt_waiter_publish(&waiter, &waiter_thread), &waiter_thread);
}

void kernel_main(void){ /* Exercise interrupt-wait publication and race arbitration. */
  say("***interrupt waiter test start\n", NULL);

  test_signal_before_publish();
  say("***interrupt waiter signal-before-publish ok\n", NULL);

  test_publish_before_signal();
  say("***interrupt waiter publish-before-signal ok\n", NULL);

  test_prepare_clears_stale_signal();
  say("***interrupt waiter prepare ok\n", NULL);

  test_prepublication_signals_coalesce();
  say("***interrupt waiter coalescing ok\n", NULL);

  say("***interrupt waiter test complete\n", NULL);
}
