/*
 * Boot the actual MicroRealm user binary with its deterministic guest engine
 * check. This runs in headless mode and verifies exec, C code generation,
 * entity simulation, combat, and record persistence in the full emulator.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged game self-check. */
int main(void) {
  char* args[3];
  args[0] = "microrealm";
  args[1] = "--selftest";
  args[2] = NULL;
  execv("/sbin/microrealm", 2, args);
  puts("***FAIL microrealm guest exec /sbin/microrealm\n");
  return 1;
}
