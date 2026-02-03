#include "debug.h"
#include "machine.h"
#include "constants.h"
#include "print.h"

void panic(char* msg) {
  // print panic message
  spin_lock_get(&print_lock);
  puts("| KERNEL PANIC (Core ");
  unsigned core_id = get_core_id();
  print_unsigned(core_id);
  puts("): ");
  puts(msg);
  puts("| System halted.\n");
  spin_lock_release(&print_lock);

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
