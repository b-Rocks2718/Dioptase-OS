#include "print.h"
#include "ext.h"
#include "sys.h"
#include "debug.h"

// Run /sbin/init from the root filesystem
int kernel_main(void) {
  struct Node* init = node_find(&fs.root, "/sbin/init");
  if (init == NULL) {
    panic("Could not find /sbin/init\n");
  }

  say("***Running /sbin/init\n", NULL);
  int rc = run_user_program(init);

  say("***/sbin/init returned %d\n", &rc);

  return 42;
}
