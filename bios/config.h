#ifndef BIOS_CONFIG_H
#define BIOS_CONFIG_H

// Build-time BIOS configuration populated by bios/config.s.
// Fields are sourced from Makefile defines and are read-only at runtime.
struct Config {
  // Number of cores to initialize/run in the emulator.
  unsigned num_cores;
  // Non-zero when EMU_VGA is enabled in the Makefile.
  unsigned use_vga;
};

// Global BIOS configuration instance emitted by bios/config.s.
extern struct Config CONFIG;

#endif // BIOS_CONFIG_H
