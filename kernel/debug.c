#include "debug.h"
#include "machine.h"
#include "constants.h"
#include "print.h"

void panic(char* msg) {
  // print panic message
  preempt_spin_lock_acquire(&print_lock);
  puts_uart("| KERNEL PANIC (Core ");
  unsigned core_id = get_core_id();
  print_unsigned_uart(core_id);
  puts_uart("): ");
  puts_uart(msg);
  puts_uart("| System halted.\n");
  preempt_spin_lock_release(&print_lock);

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
