#include "../kernel/vmem.h"
#include "../kernel/print.h"

/*
 * Smoke test: broadcast an IPI and check that each core's ipi_handler logs it.
 * IPIs carry no payload; any data would be passed through shared memory.
 * There is no .ok baseline because handler output interleaves across cores.
 */
void kernel_main(void){
  say("***Sending test IPI\n", NULL);
  send_ipi();
  say("***Test IPI sent\n", NULL);
}
