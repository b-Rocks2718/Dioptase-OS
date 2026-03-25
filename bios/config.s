  # Build-time BIOS configuration blob
  # Makefile passes in NUM_CORES and USE_VGA to assembler
  .align 4
  .data
  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA

  .text
