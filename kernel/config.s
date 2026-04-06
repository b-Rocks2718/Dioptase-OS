  .data
  # Align after selecting .data. A prior file can end its data contribution on an
  # odd byte boundary because compiler-emitted string literals are byte-sized.
  .align 4

  # Compile time configuration constants
  # These are macros, the Makefile defines them
  # and passes them to the compiler/assembler as -D flags

  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA

  
