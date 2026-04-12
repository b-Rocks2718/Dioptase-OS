  .text
  .align 4

  .global trap_handler_
trap_handler_:
  # Trap ABI on entry:
  # - r1 holds the user-selected trap code.
  # - r2-r8 hold trap-specific arguments.

  # Trap entry already cleared IMR[31]. Snapshot the interrupted PC/flags
  # before re-enabling nested interrupts so rfe later restores the exact trap
  # entry state.
  mov  r9, epc # r9, r10 are caller saved and do not contain any arguments
  push r9
  mov  r9, efg
  push r9

  # Save C frame linkage before calling into kernel C code
  push bp
  push ra

  # Re-enable interrupts after the trap frame is fully saved
  movi r9, 0x80000000
  mov  r10, imr
  or   r10, r9, r10
  mov  imr, r10

  call trap_handler

  # Disable the global interrupts again before restoring EPC/EFG and
  # returning through rfe
  movi r9, 0x7FFFFFFF
  mov  r10, imr
  and  r10, r9, r10
  mov  imr, r10

  pop  ra
  pop  bp

  pop  r9
  mov  efg, r9
  pop  r9
  mov  epc, r9

  rfe

  .global jump_to_user
jump_to_user:
  # disable interrupts before setting up epc and efg
  movi r9, 0x7FFFFFFF
  mov  r10, imr
  and  r10, r9, r10
  mov  imr, r10
  # interrupts will be restored on rfe

  # set up user stack
  crmv sp, r2

  # set up user program counter
  mov epc, r1

  # clear flags
  mov efg, r0

  rfe
