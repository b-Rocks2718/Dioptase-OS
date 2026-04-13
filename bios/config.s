  # Build-time BIOS configuration blob
  # Makefile passes in NUM_CORES, USE_VGA, and USE_AUDIO to assembler
  .data
  # Align after selecting .data so earlier byte-sized data does not misalign
  # this word-addressed configuration blob.
  .align 4
  .global CONFIG
CONFIG:
  .fill NUM_CORES
  .fill USE_VGA
  .fill USE_AUDIO

  .text
