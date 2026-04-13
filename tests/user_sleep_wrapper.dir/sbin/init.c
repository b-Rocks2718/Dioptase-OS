/*
 * user_sleep_wrapper guest:
 * - force a known stale value into r2 immediately before calling the sleep
 *   wrapper
 * - confirm the wrapper still sleeps for the requested duration rather than
 *   forwarding that stale caller-saved register value to the kernel
 * - return 0 on success so the kernel-side harness can compare exact output
 */

#include "../../../crt/sys.h"

#define REQUESTED_SLEEP_JIFFIES 5

extern void sleep_with_stale_r2(unsigned jiffies);

int main(void){
  unsigned start = get_current_jiffies();
  sleep_with_stale_r2(REQUESTED_SLEEP_JIFFIES);
  unsigned elapsed = get_current_jiffies() - start;

  if (elapsed < REQUESTED_SLEEP_JIFFIES) {
    return 1;
  }

  return 0;
}
