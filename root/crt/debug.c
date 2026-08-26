#include "debug.h"

#include "assert.h"
#include "print.h"
#include "stdlib.h"

void panic(char* msg) {
  // print panic message
  puts("USER PANIC: ");
  puts(msg);
  puts("Program exiting\n");

  // halt the system
  while (true) {
    exit(-1);
  }
}

void assert(bool condition, char* msg) {
#ifdef OS_RELEASE
  (void)condition;
  (void)msg;
#else
  if (!condition) {
    panic(msg);
  }
#endif
}

void assert_always(bool condition, char* msg) {
  if (!condition) {
    panic(msg);
  }
}
