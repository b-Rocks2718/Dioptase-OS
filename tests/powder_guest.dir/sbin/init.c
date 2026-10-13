/*
 * Boot the shipping Powderworks binary with its deterministic guest check.
 * Runs without a VGA host and verifies exec, cellular rules, display
 * mappings, audio ownership, and scene persistence in the full emulator.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged game self-check. */
int main(void) {
  char* args[3];
  args[0] = "powder";
  args[1] = "--selftest";
  args[2] = NULL;
  execv("/sbin/powder", 2, args);
  puts("***FAIL powder guest exec /sbin/powder\n");
  return 1;
}
