
# Physmem address range: 0x00000 - 0x7FFFFFF

.define KERNEL_STACK_TOP, 0x7FFFFF0

# kernel entry point
  .global _start
_start:
  # initialize kernel stack
  movi sp, KERNEL_STACK_TOP

  call kernel_entry

  # halt the CPU if kernel_entry returns
  mode halt
