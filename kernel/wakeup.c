#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "config.h"

static int awake_cores = 0;

void wakeup_all(void) {
  get_spinlock(&print_lock);

  puts("core ");
  print_num(get_core_id());
  puts(" awake\n");

  release_spinlock(&print_lock);

  __atomic_fetch_add(&awake_cores, 1);

  // get number of cores from config
  int num_cores = CONFIG[0];
   
  if (get_core_id() == 0) {

    // initialize other cores
    for (int i = 1; i < num_cores; ++i) {
      wakeup_core(i);
    }
  }

  // wait until all cores are awake
  while (awake_cores < num_cores);
}