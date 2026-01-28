
# Physmem address range: 0x0000000 - 0x7FFFFFF

.define KERNEL_STACK_TOP, 0x400000
.define KERNEL_STACK_SIZE, 0x10000
.define IPI_IVT_OFFSET, 0x3D4

# kernel entry point
  .global _start
_start:
  # register ipi handler
  adpc r1, ipi_handler_
  movi r2, IPI_IVT_OFFSET
  swa  r1, [r2]

  # initialize kernel stack
  mov  r1, cid # get core id
  movi r2, KERNEL_STACK_SIZE
  movi sp, KERNEL_STACK_TOP
  call umul
  sub  sp, sp, r1

  call wakeup_all

  call kernel_entry

  mode halt


ipi_handler_:
  jmp _start