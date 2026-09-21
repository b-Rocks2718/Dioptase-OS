#include "debug.h"

#include "assert.h"
#include "print.h"
#include "stdlib.h"

// Print a fatal diagnostic and stop the current user program.
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

// Panic on a failed condition in debug builds; compile to a no-op in releases.
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

// Panic on a failed condition in every build configuration.
void assert_always(bool condition, char* msg) {
  if (!condition) {
    panic(msg);
  }
}
