#include "kernel/machine.h"
#include "kernel/print.h"
#include "kernel/atomic.h"
#include "kernel/config.h"


int spinlock = 0;

int spinlock_counter = 0;
int atomic_counter = 0;
int unsafe_counter = 0;

int spinlock_test(void){
  // test spinlocks by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    get_spinlock(&spinlock);
    spinlock_counter++;
    release_spinlock(&spinlock);
  }
}

int atomic_test(void){
  // test atomics by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    __atomic_fetch_add(&atomic_counter, 1);
  }
}

int unsafe_test(void){
  // test unsafe increments by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    unsafe_counter++;
  }
}

int kernel_entry(void) {
  spinlock_test();

  atomic_test();

  unsafe_test();

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