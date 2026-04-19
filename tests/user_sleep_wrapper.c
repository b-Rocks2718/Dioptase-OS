/*
 * user_sleep_wrapper:
 * - boot a user /sbin/init program and treat its exit status as the test result
 * - verify the user-space sleep syscall wrapper preserves its argument across
 *   the trap ABI register shuffle
 * - catch regressions where the wrapper accidentally forwards stale r2 instead
 *   of the requested sleep duration
 */

#include "../kernel/print.h"
#include "../kernel/ext.h"
#include "../kernel/vmem.h"
#include "../kernel/elf.h"
#include "../kernel/sys.h"

int kernel_main(void){
  say("***Running /sbin/init\n", NULL);

  struct Node* init = node_find(&fs.root, "/sbin/init");
  int rc = run_user_program(init, 0, NULL);

  say("***/sbin/init returned %d\n", &rc);

  return 42;
}
