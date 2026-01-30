  .align 4
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
  # Preconditions: IMR top bit already cleared by HW
  # so nested interrupts are disabled on entry; stack pointer (r31/ksp) valid.
  # Postconditions: ISR status bit cleared by callee; IMR top bit re-enabled by rfi.

  # Save GPRs. Do not push r31: it is the stack pointer in kernel mode.
  push r1
  push r2
  push r3
  push r4
  push r5
  push r6
  push r7
  push r8
  push r9
  push r10
  push r11
  push r12
  push r13
  push r14
  push r15
  push r16
  push r17
  push r18
  push r19
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push r29
  push r30

  call pit_handler

  # Restore GPRs in reverse order.
  pop r30
  pop r29
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20
  pop r19
  pop r18
  pop r17
  pop r16
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop r7
  pop r6
  pop r5
  pop r4
  pop r3
  pop r2
  pop r1

  rfi # return address should still be in epc, flags in efg
