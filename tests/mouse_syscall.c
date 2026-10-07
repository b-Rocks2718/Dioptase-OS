/*
 * PS/2 mouse user-interface test.
 *
 * Runs /sbin/init from mouse_syscall.dir, which checks from user mode that:
 * - trap 57 (getmouse) reaches the kernel and returns 0 when no mouse event
 *   is queued (headless runs have no host mouse)
 * - root/crt/mouse.h decodes every field of a mouse word, including the
 *   sign extension of negative DX/DY/WHEEL bytes
 * The program returns 42 only if every check passed; any other value is the
 * number of the first failed check.
 */

#include "../kernel/print.h"
#include "../kernel/ext.h"
#include "../kernel/vmem.h"
#include "../kernel/elf.h"
#include "../kernel/sys.h"

int kernel_main(void){ /* Run the user-mode mouse interface checks. */
  say("***Running /sbin/init\n", NULL);

  struct Node* init = node_find(&fs.root, "/sbin/init");
  int rc = run_user_program(init, 0, NULL);

  say("***/sbin/init returned %d\n", &rc);

  return 42;
}
