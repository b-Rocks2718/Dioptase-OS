
# Physmem address range: 0x0000000 - 0x7FFFFFF

.define KERNEL_STACK_TOP, 0x100000
.define KERNEL_STACK_SIZE, 0x4000
.define IPI_IVT_OFFSET, 0x3D4

# Section load addresses must match the kernel memory map so pc-relative
# immediates resolve to runtime addresses.
  .text_load TEXT_LOAD_ADDR
  .rodata_load RODATA_LOAD_ADDR
  .data_load DATA_LOAD_ADDR
  .bss_load BSS_LOAD_ADDR

# kernel entry point
  .text
  .global _start
_start:
  # set imr to disable all interrupts
  mov imr, r0

  # clear isr so no pending interrupts
  mov  isr, r0

  # ensure kernel depth is 1
  movi r1, 1
  mov  psr, r1

  mov  r1, cid
  cmp  r1, r0
  bnz  skip_handler_register # only core 0 needs to register the ipi handler

  # register ipi handler so we can wake up other cores
  adpc r1, ipi_handler_
  movi r2, IPI_IVT_OFFSET
  swa  r1, [r2]

skip_handler_register:
  # initialize kernel stack (use cid to offset stacks for each core)
  mov  r1, cid # get core id
  movi r2, KERNEL_STACK_SIZE
  movi sp, KERNEL_STACK_TOP
  call umul
  sub  sp, sp, r1

  call kernel_entry # all cores call kernel_entry, which should never return

  mode halt


ipi_handler_:
  # Clear pending IPI so we don't immediately re-enter.
  mov isr, r0

  # Ensure we return to kernel mode after rfi.
  # Interrupt entry incremented PSR to 1 for secondary cores, so set it to 2
  # so rfi leaves PSR=1 and kmode=true.
  movi r1, 2
  mov  psr, r1

  # Return into _start using rfi so ISP depth is unwound.
  adpc r1, _start
  mov  epc, r1
  mov  efg, r0
  rfi
