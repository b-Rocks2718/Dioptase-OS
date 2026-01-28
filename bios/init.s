
# Physmem address range: 0x0000000 - 0x7FFFFFF

.define BIOS_STACK_TOP, 0x400000

# bios entry point
  .origin 0x400
  .global _start
_start:

  # initialize bios stack
  mov  r1, cid # get core id
  movi sp, BIOS_STACK_TOP

  call bios_entry

  mode halt
  