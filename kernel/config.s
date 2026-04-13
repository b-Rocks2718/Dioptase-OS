  .data
  # Align after selecting .data. A prior file can end its data contribution on an
  # odd byte boundary because compiler-emitted string literals are byte-sized.
  .align 4

  # Compile time configuration constants
  # These are macros. The Makefile defines them and passes them to the
  # assembler as -D flags, so the emitted CONFIG layout must match
  # kernel/config.h exactly.

  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA
  .fill USE_AUDIO

  
