#include "constants.h"

// Compile time configuration constants
struct Config {
  unsigned num_cores;
  bool use_vga; // otherwise use UART for output
};

extern struct Config CONFIG;
