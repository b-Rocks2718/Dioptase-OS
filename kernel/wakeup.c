#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "config.h"

static int awake_cores = 0;

int wakeup_all(void) {
  get_spinlock(&print_lock);

  // print "core <n> awake\n";
  putchar(99); // c
  putchar(111); // o
  putchar(114); // r
  putchar(101); // e
  putchar(32); // space
  print_num(get_core_id());
  putchar(32); // space
  putchar(97); // a
  putchar(119); // w
  putchar(97); // a
  putchar(107); // k
  putchar(101); // e
  putchar(10); // newline

  release_spinlock(&print_lock);

  __atomic_fetch_add(&awake_cores, 1);

  // get number of cores from config
  int num_cores = CONFIG; // CONFIG[0] once i get arrays working
  
  if (get_core_id() == 0) {

    // initialize other cores
    for (int i = 1; i < num_cores; ++i) {
      wakeup_core(i);
    }
  }

  // wait until all cores are awake
  while (awake_cores < num_cores);

  return 0;
}