#include "../kernel/print.h"
#include "../kernel/ext.h"
#include "../kernel/vmem.h"
#include "../kernel/elf.h"
#include "../kernel/sys.h"

int kernel_main(void){
  say("***Running /sbin/init\n", NULL);

  struct Node* init = node_find(&fs.root, "/sbin/init");
  int rc = run_user_program(init);

  say("***/sbin/init returned %d\n", &rc);

  return 42;
}
