  .align 4
  .text
  
  # Perform a context switch from the current thread (me) to the next thread (next)
  # Run func(arg) in the context of the next thread before switching to it
  # Needs pointer to current_thread entry of the core's PerCore struct
  # Assumes interrupts are disabled when this is called, and will re-enable them in the new thread's context
  # Assumes callback doesn't modify the 'next' TCB

  # r1 = struct TCB* me
  # r2 = struct TCB* next
  # r3 = void (*func)(void *)
  # r4 = void *arg
  # r5 = struct TCB** cur_thread
  # r6 = int was
  # r7 = bool run_with_interrupts
  .global context_switch
context_switch:
  # save current state
  swa  r20, [r1, 76]
  swa  r21, [r1, 80]
  swa  r22, [r1, 84]
  swa  r23, [r1, 88]
  swa  r24, [r1, 92]
  swa  r25, [r1, 96]
  swa  r26, [r1, 100]
  swa  r27, [r1, 104]
  swa  r28, [r1, 108]

  swa  sp,  [r1, 112]
  swa  bp,  [r1, 116]

  # use r9 as scratch
  mov  r9, flg
  swa  r9,  [r1, 120]

  swa  ra,  [r1, 124]

  mov  r9, psr
  swa  r9,  [r1, 128]

  # TCB offset 132 stores per-thread IMR state
  # The caller passes the outgoing thread's pre-switch IMR in r6 ("was")
  swa  r6,  [r1, 132]

  lwa  r9, [r2, 128]
  mov  psr, r9

  # restore old state
  lwa  r20, [r2, 76]
  lwa  r21, [r2, 80]
  lwa  r22, [r2, 84]
  lwa  r23, [r2, 88]
  lwa  r24, [r2, 92]
  lwa  r25, [r2, 96]
  lwa  r26, [r2, 100]
  lwa  r27, [r2, 104]
  lwa  r28, [r2, 108]

  lwa  sp,  [r2, 112]
  lwa  bp,  [r2, 116]

  # use r10 as scratch
  lwa  r10, [r2, 120]
  mov  flg, r10

  lwa  r10, [r2, 124] # r10 holds our return address
  push r10

  # update current thread
  swa  r2,  [r5]

  # check if we should restore interrupts before the callback
  cmp  r7, r0
  bz   context_switch_no_interrupts

	# restore interrupt flags
  lwa  r9,  [r2, 132]
  mov  imr, r9

context_switch_no_interrupts:
  push r2 # save/restore r2 across the callback so we can still read the TCB after it returns

  # call the function we were passed, after the switch
  mov  r1, r4
  bra  ra, r3

  pop r2 

  # restore interrupts after callback regardless of run_with_interrupts
  # since either way we want interrupts restored at this point
  lwa  r9,  [r2, 132]
  mov  imr, r9

  pop  ra

	ret