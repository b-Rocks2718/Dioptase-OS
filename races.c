#include "kernel/machine.h"
#include "kernel/config.h"
#include "kernel/print.h"

int kernel_entry(void) {
  // print core num that woke up
  print_num(get_core_id());
  putchar(10); // newline

  // get number of cores from config
    int num_cores = CONFIG; // CONFIG[0] once i get arrays working
  
  if (get_core_id() == 0) {

    // initialize other cores
    for (int i = 1; i < num_cores; ++i) {
      wakeup_core(i);
    }
  }

  if (get_core_id() != num_cores - 1) {
    while (1); // other cores just loop forever for now
  }

  return 67;
}