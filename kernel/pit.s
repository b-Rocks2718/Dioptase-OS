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
  sisa  r1, [0]
  sisa  r2, [4]
  sisa  r3, [8]
  sisa  r4, [12]
  sisa  r5, [16]
  sisa  r6, [20]
  sisa  r7, [24]
  sisa  r8, [28]
  sisa  r9, [32]
  sisa  r10, [36]
  sisa  r11, [40]
  sisa  r12, [44]
  sisa  r13, [48]
  sisa  r14, [52]
  sisa  r15, [56]
  sisa  r16, [60]
  sisa  r17, [64]
  sisa  r18, [68]
  sisa  r19, [72]

  mov  r1, epc
  sisa  r1, [76] # save return address

  mov  r1, efg
  sisa  r1, [80] # save flags

  mov  r1, ksp # save kernel stack pointer
  sisa  r1, [84]

  # save bp and ra
  sisa  bp,  [88]
  sisa  ra,  [92]

  mov  r1, isa # pass save area to pit_handler

  # align stack to 4 bytes
  movi r2, 0xFFFFFFFC
  and  sp, r2, sp

  call pit_handler # returns TCB* in r1

  # r1 contains TCB* so we can restore everything
  lwa  r2, [r1, 136] # restore return address
  mov  epc, r2

  lwa  r2, [r1, 140] # restore flags
  mov  efg, r2

  # restore kernel stack pointer
  lwa  r2, [r1, 144]
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
  lwa  bp,  [r1, 148]
  lwa  ra,  [r1, 152]

  lwa  r1, [r1, 0] # last so we don't clobber r1 early

  rfi


.global fake_pit_handler
fake_pit_handler:
  # fake pit handler used for testing

  # Save caller-saved registers
  sisa  r1, [0]
  sisa  r2, [4]
  sisa  r3, [8]
  sisa  r4, [12]
  sisa  r5, [16]
  sisa  r6, [20]
  sisa  r7, [24]
  sisa  r8, [28]
  sisa  r9, [32]
  sisa  r10, [36]
  sisa  r11, [40]
  sisa  r12, [44]
  sisa  r13, [48]
  sisa  r14, [52]
  sisa  r15, [56]
  sisa  r16, [60]
  sisa  r17, [64]
  sisa  r18, [68]
  sisa  r19, [72]

  mov  r1, epc
  sisa  r1, [76] # save return address

  mov  r1, efg
  sisa  r1, [80] # save flags

  mov  r1, ksp # save kernel stack pointer
  sisa  r1, [84]

  # save bp and ra
  sisa  bp,  [88]
  sisa  ra,  [92]

  mov  r1, isa # pass save area to pit_handler

  # align stack to 4 bytes
  movi r2, 0xFFFFFFFC
  and  sp, r2, sp

  call pit_handler # returns TCB* in r1

  # r1 contains TCB* so we can restore everything
  lwa  r2, [r1, 136] # restore return address
  mov  epc, r2

  lwa  r2, [r1, 140] # restore flags
  mov  efg, r2

  # restore kernel stack pointer
  lwa  r2, [r1, 144]
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
  lwa  bp,  [r1, 148]
  lwa  ra,  [r1, 152]

  lwa  r1, [r1, 0] # last so we don't clobber r1 early

  ret
