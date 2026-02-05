#include "constants.h"

struct Config {
  unsigned num_cores;
  bool use_vga; // otherwise use UART for output
};

extern struct Config CONFIG;
