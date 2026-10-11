/*
 * Run the shipped Lumen executable's deterministic self-test in the full OS.
 * This catches guest compiler, executable loading, and CRT integration bugs
 * that a host build of the game logic would miss. Diagnostics from Lumen stay
 * in the raw log; the checked output records its final process status.
 *
 * This program runs in user mode and uses the documented process wrappers.
 * fork() snapshots the parent's private memory; the two processes share no
 * mutable game state. wait_child() observes child termination and consumes
 * its descriptor before the parent reports success, on any core count.
 */

#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"

// Require a bounded mode of the installed executable to exit successfully.
static int run_mode(char* mode) {
  int child = fork();

  if (child < 0) {
    int args[1] = {child};
    printf("***FAIL lumen validation: fork returned %d; expected a child descriptor\n", args);
    return 1;
  }

  if (child == 0) {
    char* argv[2] = {"/sbin/lumen", mode};
    int rc = execv("/sbin/lumen", 2, argv);
    int args[1] = {rc};
    printf("***FAIL lumen validation: execv /sbin/lumen returned %d; expected image replacement\n", args);
    return 1;
  }

  int status = wait_child(child);
  if (status != 0) {
    int args[2] = {child, status};
    printf("***FAIL lumen validation: child descriptor %d exited %d; expected 0\n", args);
    return 1;
  }

  return 0;
}

// Verify rules and then exercise real display mappings and captured pixel output.
int main(void) {
  if (run_mode("--selftest")) return 1;
  puts("***PASS lumen selftest exited 0\n");
  if (run_mode("--capture")) return 1;
  /* Capture hides the cursor without a trailing newline; start a fresh log line. */
  puts("\n***PASS lumen display capture exited 0\n");
  return 0;
}
