/*
 * Boot the actual Starfall user binary with its deterministic guest engine
 * check. This runs without a VGA host and verifies exec, C code generation,
 * combat, and wave transitions in the full emulator.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged game self-check. */
int main(void) {
  char* args[3];
  args[0] = "starfall";
  args[1] = "--selftest";
  args[2] = NULL;
  execv("/sbin/starfall", 2, args);
  puts("***FAIL starfall guest exec /sbin/starfall\n");
  return 1;
}
