/*
 * pin_core() test (trap 64):
 * - out-of-range cores are rejected with -1 and change nothing
 * - pinning returns the requested core, and a pinned thread that blocks
 *   (sleep) is woken on that same core: pin_core(-1) returns the core the
 *   caller is running on, so calling it after the sleep both unpins and
 *   reports where the thread resumed
 * - a forked child inherits the pin: the child's first pin_core(-1) reports
 *   the parent's core, which the child returns as its exit status
 *
 * Needs at least three cores (it pins to cores 1 and 2), so a pin that is
 * ignored cannot pass by landing on core 0 by chance.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

int main(void){ /* Verify pin_core() bounds, blocking behavior, and fork inheritance. */
  user_test_expect_eq("reject core -2", pin_core(-2), -1);
  user_test_expect_eq("reject core 1000", pin_core(1000), -1);

  user_test_expect_eq("pin to core 2", pin_core(2), 2);
  sleep(3);
  user_test_expect_eq("woken on the pinned core", pin_core(-1), 2);

  user_test_expect_eq("pin to core 1", pin_core(1), 1);
  int child = fork();
  if (child == 0){
    exit(pin_core(-1));
  }
  user_test_expect_eq("child inherits the pin", wait_child(child), 1);
  sleep(3);
  user_test_expect_eq("parent still on core 1", pin_core(-1), 1);
  return 0;
}
