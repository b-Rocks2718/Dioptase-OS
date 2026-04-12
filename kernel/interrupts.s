  .text
  .align 4
  
  # disables interrupts and returns previous imr value
  .global interrupts_disable
interrupts_disable:
  # return previous state
  mov  r1, imr
  mov  imr, r0
  ret
  
  # restores imr to previous state, returns value that was in imr
  .global interrupts_restore
interrupts_restore:
  # r1 has mask to restore
  mov  r2, imr # save previous state
  mov  imr, r1
  mov  r1, r2 # return previous interrupt state
  ret
