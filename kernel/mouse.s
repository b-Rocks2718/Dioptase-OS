  .text
  .align 4

  # Mark the current mouse interrupt as handled, allowing the device to
  # route another mouse interrupt (docs/ISA.md: mouse is ISR bit 8, IVT 0xF8).
  # Kernel mode, called from mouse_handler with interrupts disabled.
  .global mark_mouse_handled
mark_mouse_handled:
  # Acknowledge only the mouse bit in ISR.
  # `eoi` performs the clear atomically with respect to new pending interrupts.
  eoi 8
  ret

  .global mouse_handler_
mouse_handler_:
  # ISR wrapper for PS/2 mouse interrupts that preserves interrupted CPU state.
  # Entry: kernel mode, IMR[31] cleared by hardware, EPC/EFG hold the
  # interrupted context. Interrupts are re-enabled by rfe.
  # Saves every caller-saved register plus epc/efg/bp/ra because the
  # interrupted code made no call; mouse_handler (C ABI) preserves the rest.
  # ISR status bit must be cleared by mouse_handler.

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

  call mouse_handler

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
