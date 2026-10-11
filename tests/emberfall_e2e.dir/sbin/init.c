/*
 * Boot the shipping Emberfall binary with its scripted end-to-end pass.
 * Runs without a VGA host: keys travel as raw stdin words, framebuffers
 * are read back from real MMIO, and stats plus dumps land on SD1 output.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged game e2e check. */
int main(void) {
  char* args[3];
  args[0] = "emberfall";
  args[1] = "--e2e";
  args[2] = NULL;
  execv("/sbin/emberfall", 2, args);
  puts("***FAIL emberfall e2e exec /sbin/emberfall\n");
  return 1;
}
