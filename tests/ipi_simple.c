#include "../kernel/vmem.h"
#include "../kernel/print.h"
#include "../kernel/ivt.h"
#include "../kernel/machine.h"

void simple_ipi_handler() {
  unsigned data = get_mbi();
  mark_ipi_handled();

  int cid = get_core_id();
  int args[2] = {cid, data};
  say("| Received IPI on core %d with data %d\n", args);
}

void kernel_main(void) {
  register_handler((void*)simple_ipi_handler, (void*)IPI_IVT_ENTRY);
  say("***Sending test IPI with data 42\n", NULL);
  send_ipi(42);
  say("***Test IPI sent\n", NULL);
}
