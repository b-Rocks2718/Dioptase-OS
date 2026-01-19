#include "kernel/machine.h"
#include "kernel/print.h"
#include "kernel/atomic.h"

int spinlock = 0;

int spinlock_counter = 0;
int atomic_counter = 0;
int unsafe_counter = 0;

int kernel_entry(void) {
  // tests have multiple cores increment a shared counter

  // test spinlocks
  for (int i = 0; i < 1000; ++i) {
    get_spinlock(&spinlock);
    spinlock_counter++;
    release_spinlock(&spinlock);
  }

  // test atomics
  for (int i = 0; i < 1000; ++i) {
    __atomic_fetch_add(&atomic_counter, 1);
  }

  // test unsafe increments
  for (int i = 0; i < 1000; ++i) {
    unsafe_counter++;
  }

  if (get_core_id() == 0) {
    puts("spinlock_counter: ");
    print_num(spinlock_counter);
    putchar('\n');

    puts("atomic_counter: ");
    print_num(atomic_counter);
    putchar('\n');

    puts("unsafe_counter: ");
    print_num(unsafe_counter);
    putchar('\n');
  } else {
    while (1);
  }

  return 67;
}