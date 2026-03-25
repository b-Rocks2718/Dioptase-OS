  .align 4
  .data

  # Compile time configuration constants
  # These are macros, the Makefile defines them
  # and passes them to the compiler/assembler as -D flags

  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA

  