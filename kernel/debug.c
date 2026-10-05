#include "debug.h"
#include "machine.h"
#include "constants.h"
#include "print.h"

// Halt the kernel after reporting an unrecoverable invariant violation.
void panic(char* msg) {
  // print panic message
  puts_uart("| KERNEL PANIC (Core ");
  unsigned core_id = get_core_id();
  print_unsigned_uart(core_id);
  puts_uart("): ");
  puts_uart(msg);
  puts_uart("| System halted.\n");

  // halt the system
  while (true) {
    shutdown();
  }
}

// Print the failing assertion's location over raw UART, then panic. Raw UART
// output bypasses console serialization in case this core owns the console.
void assert_failed(char* file, int line, char* msg) {
  puts_uart("| assertion failed at ");
  puts_uart(file);
  putchar_uart(':');
  print_signed_uart(line);
  putchar_uart('\n');
  panic(msg);
}
