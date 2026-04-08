#include "../kernel/vmem.h"
#include "../kernel/print.h"

void kernel_main(void){
  say("***Sending test IPI with data 42\n", NULL);
  send_ipi(42);
  say("***Test IPI sent\n", NULL);
}
