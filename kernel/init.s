
  # Physmem address range: 0x0000000 - 0x7FFFFFF

  .define KERNEL_STACK_TOP, 0x100000
  .define KERNEL_STACK_SIZE, 0x4000
  .define IPI_IVT_OFFSET, 0x3D4

  .text_load TEXT_LOAD_ADDR
  .rodata_load RODATA_LOAD_ADDR
  .data_load DATA_LOAD_ADDR
  .bss_load BSS_LOAD_ADDR

  .data
  .align 4

  # kernel entry point
  .text
  .align 4
  .global _start
_start:
  # set imr to disable all interrupts
  mov imr, r0

  # clear isr so no pending interrupts
  eoi all

  # ensure kernel depth is 1
  movi r1, 1
  mov  psr, r1

  # initialize kernel stack (use cid to offset stacks for each core)
  mov  r1, cid # get core id
  movi r2, KERNEL_STACK_SIZE
  movi sp, KERNEL_STACK_TOP
  call umul
  sub  sp, sp, r1

  call kernel_entry # all cores call kernel_entry, which should never return

  mode halt

  .global boot_ipi_handler_
boot_ipi_handler_:
  # Clear pending IPI so we don't immediately re-enter.
  eoi 5

  # disable interrupts
  mov imr, r0

  # Ensure we return to kernel mode after rfe.
  # Interrupt entry incremented PSR to 1 for secondary cores, so set it to 2
  # so rfe leaves PSR=1 and kmode=true.
  movi r1, 2
  mov  psr, r1

  # Return into _start using rfe so ISP depth is unwound.
  adpc r1, _start
  mov  epc, r1
  mov  efg, r0
  rfe
