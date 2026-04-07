#include "../kernel/vmem.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/heap.h"

int kernel_main(void) {
  say("***Hello from vmem_simple test!\n", NULL);

  unsigned* fault_addr = (unsigned*)0xF0000000;

  unsigned x = *fault_addr;

  say("*** read %d from unmapped addr\n", &x);

  *fault_addr = 1234;

  say("*** wrote 1234 to unmapped addr\n", NULL);

  unsigned y = *fault_addr;

  say("*** read %d from previously unmapped addr\n", &y);

  unsigned z = *test_page;

  say("*** read %d from test_page\n", &z);

  return 0;
}
