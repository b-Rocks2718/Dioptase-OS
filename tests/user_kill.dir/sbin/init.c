/*
 * user_kill guest:
 * - validate invalid child descriptors return -1 from signal_child()
 * - validate signal_child() terminates a fork child before it reaches its normal exit
 *   path
 * - validate wait_child() reports the killed-child result once and then
 *   consumes the descriptor
 *
 * How:
 * - fork one child that repeatedly yields before its normal return so the
 *   parent has time to issue signal_child() without relying on a one-instruction race
 * - have the parent reject one low and one high invalid descriptor
 * - kill the live child, verify wait_child() reports -1 for the killed child,
 *   then verify the descriptor is consumed for both wait_child() and signal_child()
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define MIN_CHILD_DESCRIPTOR 200
#define MAX_CHILD_DESCRIPTOR_EXCLUSIVE 300
#define INVALID_CHILD_DESCRIPTOR_LOW 199
#define INVALID_CHILD_DESCRIPTOR_HIGH 300

#define CHILD_YIELD_COUNT 256
#define CHILD_SHOULD_NOT_EXIT_STATUS 73

static int child_main(void){
  for (int i = 0; i < CHILD_YIELD_COUNT; ++i){
    yield();
  }

  return CHILD_SHOULD_NOT_EXIT_STATUS;
}

int main(void){
  int child = -1;

  user_test_expect_eq("signal_child(INVALID_CHILD_DESCRIPTOR_LOW, SIGNAL_TERMINATE)", signal_child(INVALID_CHILD_DESCRIPTOR_LOW, SIGNAL_TERMINATE), -1);
  user_test_expect_eq("signal_child(INVALID_CHILD_DESCRIPTOR_HIGH, SIGNAL_TERMINATE)", signal_child(INVALID_CHILD_DESCRIPTOR_HIGH, SIGNAL_TERMINATE), -1);

  child = fork();
  if (child == 0){
    return child_main();
  }

  user_test_expect_eq("child >= MIN_CHILD_DESCRIPTOR && child < MAX_CHILD_DESCRIPTOR_EXCLUSIVE", child >= MIN_CHILD_DESCRIPTOR && child < MAX_CHILD_DESCRIPTOR_EXCLUSIVE, 1);
  user_test_expect_eq("signal_child(child, SIGNAL_TERMINATE)", signal_child(child, SIGNAL_TERMINATE), 0);
  user_test_expect_eq("wait_child(child)", wait_child(child), -1);
  user_test_expect_eq("wait_child(child)", wait_child(child), -1);
  user_test_expect_eq("signal_child(child, SIGNAL_TERMINATE)", signal_child(child, SIGNAL_TERMINATE), -1);

  return 0;
}
