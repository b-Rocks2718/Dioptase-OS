#ifndef CONFIG_H
#define CONFIG_H

#include "constants.h"

#define MAX_CORES 4

// Compile time configuration constants
struct Config {
  unsigned num_cores;
  bool use_vga; // otherwise use UART for output
};

extern struct Config CONFIG;

#endif // CONFIG_H
