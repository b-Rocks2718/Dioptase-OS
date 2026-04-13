#ifndef CONFIG_H
#define CONFIG_H

#include "constants.h"

#define MAX_CORES 4

// Compile-time configuration constants populated by kernel/config.s.
// Each field is emitted as a 32-bit word so this layout must stay in sync
// with the assembly-side CONFIG blob.
struct Config {
  unsigned num_cores;
  bool use_vga; // otherwise use UART for output
  bool use_audio; // non-zero when the emulator host audio sink is enabled
};

extern struct Config CONFIG;

#endif // CONFIG_H
