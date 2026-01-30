#include "debug.h"
#include "machine.h"
#include "constants.h"
#include "print.h"

void panic(char* msg) {
  // print panic message
  puts("| KERNEL PANIC: ");
  puts(msg);
  puts("| System halted.\n");

  // halt the system
  while (true) {
    shutdown();
  }
}

void assert(bool condition, char* msg) {
  if (!condition) {
    panic(msg);
  }
}
