  .align 4
  .text

  .global mark_pit_handled
mark_pit_handled:
  mov r1, isr
  movi r2, 0xFFFFFFFE # pit is lowest bit
  and r1, r2, r1
  mov isr, r1
  ret

  .global pit_handler_
pit_handler_:
  # Purpose: ISR wrapper for PIT that preserves interrupted CPU state.
  # Preconditions: IMR top bit already cleared by HW so nested interrupts are
  # disabled on entry; stack pointer (r31/ksp) is 4-byte aligned per ABI.
  # Postconditions: ISR status bit cleared by callee; IMR top bit re-enabled by rfi.
  # Invariants: save area stays on the interrupted thread's stack until it resumes.

  # Save caller-saved registers.
  push  r1
  push  r2
  push  r3
  push  r4
  push  r5
  push  r6
  push  r7
  push  r8
  push  r9
  push  r10
  push  r11
  push  r12
  push  r13
  push  r14
  push  r15
  push  r16
  push  r17
  push  r18
  push  r19

  # Save epc/efg using r1 as scratch (r1 already saved).
  mov  r1, epc
  push r1
  mov  r1, efg
  push r1

  # Save bp and ra.
  push bp
  push ra

  call pit_handler

  pop  ra
  pop  bp

  pop  r1
  mov  efg, r1
  pop  r1
  mov  epc, r1

  pop  r19
  pop  r18
  pop  r17
  pop  r16
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  r11
  pop  r10
  pop  r9
  pop  r8
  pop  r7
  pop  r6
  pop  r5
  pop  r4
  pop  r3
  pop  r2
  pop  r1

  rfi
