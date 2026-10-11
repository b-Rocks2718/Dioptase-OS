/*
 * Boot the shipping Emberfall binary with its deterministic guest check.
 * Runs without a VGA host and verifies exec, combat, upgrades, display
 * mappings, and score ordering in the full emulator. Raw keyboard mode is a
 * terminal feature, so it is exercised by emberfall_e2e's scripted pipe.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged game self-check. */
int main(void) {
  char* args[3];
  args[0] = "emberfall";
  args[1] = "--selftest";
  args[2] = NULL;
  execv("/sbin/emberfall", 2, args);
  puts("***FAIL emberfall guest exec /sbin/emberfall\n");
  return 1;
}
