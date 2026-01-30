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
  # Preconditions: IMR top bit already cleared by HW
  # so nested interrupts are disabled on entry; stack pointer (r31/ksp) valid.
  # Postconditions: ISR status bit cleared by callee; IMR top bit re-enabled by rfi.

  # Save caller-saved registers
  swa  r1, [sp, -4]
  swa  r2, [sp, -8]
  swa  r3, [sp, -12]
  swa  r4, [sp, -16]
  swa  r5, [sp, -20]
  swa  r6, [sp, -24]
  swa  r7, [sp, -28]
  swa  r8, [sp, -32]
  swa  r9, [sp, -36]
  swa  r10, [sp, -40]
  swa  r11, [sp, -44]
  swa  r12, [sp, -48]
  swa  r13, [sp, -52]
  swa  r14, [sp, -56]
  swa  r15, [sp, -60]
  swa  r16, [sp, -64]
  swa  r17, [sp, -68]
  swa  r18, [sp, -72]
  swa  r19, [sp, -76]

  mov  r1, epc
  swa  r1, [sp, -80] # save return address

  mov  r1, efg
  swa  r1, [sp, -84] # save flags

  mov  r1, ksp # save kernel stack pointer
  swa  r1, [sp, -88]

  # save bp and ra
  swa  bp,  [sp, -92]
  swa  ra,  [sp, -96]

  mov  r1, isp # pass stack pointer to pit_handler

  # switch to kernel stack
  mov  isp, ksp

  # align to 4 bytes
  movi r2, 0xFFFFFFFC
  and  sp, r2, sp

  call pit_handler

  # restore interrupt stack pointer
  # calculate from core id
  mov  r2, cid
  lsl  r2, r2, 14 # core stack size is 16KB bytes
  movi r3, 0xF0000
  sub  r3, r3, r2

  mov  isp, r3

  # r1 contains TCB* so we can restore everything
  lwa  r2, [r1, 140] # restore return address
  mov  epc, r2

  lwa  r2, [r1, 144] # restore flags
  mov  efg, r2

  # restore kernel stack pointer
  lwa  r2, [r1, 148]
  mov  ksp, r2

  lwa  r2, [r1, 4]
  lwa  r3, [r1, 8]
  lwa  r4, [r1, 12]
  lwa  r5, [r1, 16]
  lwa  r6, [r1, 20]
  lwa  r7, [r1, 24]
  lwa  r8, [r1, 28]
  lwa  r9, [r1, 32]
  lwa  r10, [r1, 36]
  lwa  r11, [r1, 40]
  lwa  r12, [r1, 44]
  lwa  r13, [r1, 48]
  lwa  r14, [r1, 52]
  lwa  r15, [r1, 56]
  lwa  r16, [r1, 60]
  lwa  r17, [r1, 64]
  lwa  r18, [r1, 68]
  lwa  r19, [r1, 72]

  # restore bp and ra
  lwa  bp,  [r1, 152]
  lwa  ra,  [r1, 156]

  lwa  r1, [r1, 0] # last so we don't clobber r1 early

  rfi
