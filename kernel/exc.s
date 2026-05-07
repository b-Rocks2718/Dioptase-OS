  .text
  .align 4

  .global invalid_instr_exc_handler_
invalid_instr_exc_handler_:
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

  # re-enable interrupts
  movi r1, 0x80000000
  mov  r2, imr
  or   r2, r1, r2
  mov  imr, r2

  # allocate space for return_to_user argument
  push r0
  mov  r1, sp # pass in pointer
  call invalid_instr_handler

  pop  r2
  cmp  r2, r0
  bz   return_to_kernel

  # disable interrupts
  movi r1, 0x7FFFFFFF
  mov  r2, imr
  and  r2, r1, r2
  mov  imr, r2

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

  rfe

  .global priv_exc_handler_
priv_exc_handler_:
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

  # re-enable interrupts
  movi r1, 0x80000000
  mov  r2, imr
  or   r2, r1, r2
  mov  imr, r2

  # allocate space for return_to_user argument
  push r0
  mov  r1, sp # pass in pointer
  call priv_instr_handler

  pop  r2
  cmp  r2, r0
  bz   return_to_kernel

  # disable interrupts
  movi r1, 0x7FFFFFFF
  mov  r2, imr
  and  r2, r1, r2
  mov  imr, r2

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

  rfe

  .global misaligned_pc_exc_handler_
misaligned_pc_exc_handler_:
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

  # re-enable interrupts
  movi r1, 0x80000000
  mov  r2, imr
  or   r2, r1, r2
  mov  imr, r2

  # allocate space for return_to_user argument
  push r0
  mov  r1, sp # pass in pointer
  call misaligned_pc_handler

  pop  r2
  cmp  r2, r0
  bz   return_to_kernel

  # disable interrupts
  movi r1, 0x7FFFFFFF
  mov  r2, imr
  and  r2, r1, r2
  mov  imr, r2

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

  rfe

return_to_kernel:
  add sp, sp, 92 # 4 + 19 = 23 regs, 23 * 4 = 92

  pop ra
  pop bp

  ret
