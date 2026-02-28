
# Physmem address range: 0x00000 - 0x3FFFF

.define BIOS_STACK_TOP, 0x10000

# bios entry point
  .origin 0x400
  .global _start
_start:

  # initialize bios stack
  mov  r1, cid # get core id
  movi sp, BIOS_STACK_TOP

  call bios_entry

  mode halt
  