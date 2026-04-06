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

  # register the given handler function for the given IVT entry
  .global register_handler
register_handler:
  # func ptr in r1
  # dest in r2
  swa r1, [r2]
  ret

  # register spurious_interrupt_handler for all IVT entries to catch unexpected interrupts and exceptions
  .global register_spurious_handlers
register_spurious_handlers:
  adpc r1, spurious_interrupt_handler_
  add  r2, r0, 4
  movi r3, 0x400
  movi r4, 0x3D4 # IPI handler, do not overwrite
spurious_loop:
  cmp  r2, r4
  bz   spurious_loop_end
  swa  r1, [r2]
spurious_loop_end:
  add  r2, r2, 4
  cmp  r2, r3
  bl   spurious_loop
  ret

  # handler for unexpected interrupts and exceptions
  # calls into C handler and then halts the core
spurious_interrupt_handler_:
  call spurious_interrupt_handler
  mode halt
