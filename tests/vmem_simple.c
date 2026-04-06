#include "../kernel/vmem.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/heap.h"

int kernel_main(void) {
  say("***Hello from vmem_simple test!\n", NULL);

  /*unsigned* fault_addr = (unsigned*)0xF0000000;

  unsigned x = *fault_addr;

  say("*** read from unmapped addr\n", NULL);

  */

  return 0;
}
