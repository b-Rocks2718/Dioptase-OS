  .text
  .align 4

  # Mark the current UART RX interrupt as handled, 
  # allowing the UART to send more interrupts
  .global mark_uart_rx_handled
mark_uart_rx_handled:
  # Acknowledge only the UART RX bit in ISR.
  # `eoi` performs the clear atomically with respect to new pending interrupts
  eoi 4
  ret

  .global uart_rx_handler_
uart_rx_handler_:
  # ISR wrapper for uart that preserves interrupted CPU state.
  # Interrupts have been disabled by hardware, will be re-enabled by rfe
  # ISR status bit must be cleared by pit_handler

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

  call uart_rx_handler

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
