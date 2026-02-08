  # Build-time BIOS configuration blob.
  # Inputs:
  # - NUM_CORES from the Makefile.
  # - USE_VGA from the Makefile (0/1).
  # Output:
  # - CONFIG symbol with fields matching bios/config.h.
  .align 4
  .data
  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA

