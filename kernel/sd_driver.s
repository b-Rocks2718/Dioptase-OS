  .align 4
  .text

  # Mark the current SD0 interrupt as handled, allowing the SD0 to send more interrupts
  .global mark_sd0_handled
mark_sd0_handled:
  # Acknowledge only the SD0 bit in ISR.
  # `eoi` performs the clear atomically with respect to new pending interrupts.
  eoi 3
  ret

  # Mark the current SD1 interrupt as handled, allowing the SD1 to send more interrupts
  .global mark_sd1_handled
mark_sd1_handled:
  # Acknowledge only the SD1 bit in ISR.
  # `eoi` performs the clear atomically with respect to new pending interrupts.
  eoi 6
  ret

  .global sd0_handler_
sd0_handler_:
  # ISR wrapper for SD0 interrupts that preserves interrupted CPU state.
  # Interrupts have been disabled by hardware, will be re-enabled by rfi
  # ISR status bit must be cleared by sd0_handler

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

  mov r1, r0 # pass 0 for SD0
  call sd_handler

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

.global sd1_handler_
sd1_handler_:
  # ISR wrapper for SD1 interrupts that preserves interrupted CPU state.
  # Interrupts have been disabled by hardware, will be re-enabled by rfi
  # ISR status bit must be cleared by sd1_handler

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

  movi r1, 1 # pass 1 for SD1
  call sd_handler

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
