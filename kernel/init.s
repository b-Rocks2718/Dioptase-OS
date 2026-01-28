
# Physmem address range: 0x0000000 - 0x7FFFFFF

.define KERNEL_STACK_TOP, 0x100000
.define KERNEL_STACK_SIZE, 0x4000
.define IPI_IVT_OFFSET, 0x3D4

# kernel entry point
  .global _start
_start:
  mov  r1, cid
  cmp  r1, r0
  bnz  skip_handler_register # only core 0 needs to register the ipi handler

  # register ipi handler
  adpc r1, ipi_handler_
  movi r2, IPI_IVT_OFFSET
  swa  r1, [r2]

skip_handler_register:
  # initialize kernel stack
  mov  r1, cid # get core id
  movi r2, KERNEL_STACK_SIZE
  movi sp, KERNEL_STACK_TOP
  call umul
  sub  sp, sp, r1

  call kernel_entry

  mode halt


ipi_handler_:
  jmp _start