#include "kernel/machine.h"
#include "kernel/print.h"
#include "kernel/atomic.h"


int spinlock = 0;

int spinlock_counter = 0;
int atomic_counter = 0;
int unsafe_counter = 0;

int core_0_done = 0;
int core_1_done = 0;
int core_2_done = 0;
int core_3_done = 0;

int wait_barrier = 0;

int im_done(void) {
  int me = get_core_id();
  if (me == 0) {
    core_0_done = 1;
  } else if (me == 1) {
    core_1_done = 1;
  } else if (me == 2) {
    core_2_done = 1;
  } else if (me == 3) {
    core_3_done = 1;
  }
  return 0;
}

int reset(void){
  core_0_done = 0;
  core_1_done = 0;
  core_2_done = 0;
  core_3_done = 0;

  wait_barrier = 0;

  return 0;
}

int wait_on_cores_done(void){
  while (!core_0_done || !core_1_done || !core_2_done || !core_3_done);
  return 0;
}


int spinlock_test(void){
  wait_barrier = 1;

  // test spinlocks by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    get_spinlock(&spinlock);
    spinlock_counter++;
    release_spinlock(&spinlock);
  }

  im_done();

  // wait for all cores to finish
  wait_on_cores_done();

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

    reset();
  } else {
    // other cores do nothing
    while (wait_barrier);
  }
}

int atomic_test(void){
  wait_barrier = 1;

  // test atomics by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    __atomic_fetch_add(&atomic_counter, 1);
  }

  im_done();

  // wait for all cores to finish
  wait_on_cores_done();

  if (get_core_id() == 0) {
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

    reset();
  } else {
    // other cores do nothing
    while (wait_barrier);
  }
}

int unsafe_test(void){
  wait_barrier = 1;

  // test unsafe increments by having all cores increment a shared counter
  for (int i = 0; i < 1000; ++i) {
    unsafe_counter++;
  }

  im_done();

  // wait for all cores to finish
  wait_on_cores_done();

  if (get_core_id() == 0) {
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

    reset();
  } else {
    // other cores do nothing
    while (wait_barrier);
  }
}

int kernel_entry(void) {

  spinlock_test();

  atomic_test();

  unsafe_test();

  return 67;
}