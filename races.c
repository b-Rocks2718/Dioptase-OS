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
    // print "spinlock_counter: <value>\n"
    putchar(115); // s
    putchar(112); // p
    putchar(105); // i
    putchar(110); // n
    putchar(108); // l
    putchar(111); // o
    putchar(99); // c
    putchar(107); // k
    putchar(95); // _
    putchar(99); // c
    putchar(111); // o
    putchar(117); // u
    putchar(110); // n
    putchar(116); // t
    putchar(101); // e
    putchar(114); // r
    putchar(58); // :
    putchar(32); // space
    print_num(spinlock_counter);
    putchar(10); // newline

    // print "atomic_counter: <value>\n"
    putchar(97); // a
    putchar(116); // t
    putchar(111); // o
    putchar(109); // m
    putchar(105); // i
    putchar(99); // c
    putchar(95); // _
    putchar(99); // c
    putchar(111); // o
    putchar(117); // u
    putchar(110); // n
    putchar(116); // t
    putchar(101); // e
    putchar(114); // r
    putchar(58); // :
    putchar(32); // space
    print_num(atomic_counter);
    putchar(10); // newline

    // print "unsafe_counter: <value>\n"
    putchar(117); // u
    putchar(110); // n
    putchar(115); // s
    putchar(97); // a
    putchar(102); // f
    putchar(101); // e
    putchar(95); // _
    putchar(99); // c
    putchar(111); // o
    putchar(117); // u
    putchar(110); // n
    putchar(116); // t
    putchar(101); // e
    putchar(114); // r
    putchar(58); // :
    putchar(32); // space
    print_num(unsafe_counter);
    putchar(10); // newline
  } else {
    while (1);
  }

  return 67;
}