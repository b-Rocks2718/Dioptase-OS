  .text
  .align 4
  
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
  swa  r20, [r1, 0]
  swa  r21, [r1, 4]
  swa  r22, [r1, 8]
  swa  r23, [r1, 12]
  swa  r24, [r1, 16]
  swa  r25, [r1, 20]
  swa  r26, [r1, 24]
  swa  r27, [r1, 28]
  swa  r28, [r1, 32]

  swa  bp,  [r1, 40]

  crmv r9, sp
  swa  r9,  [r1, 36]

  swa  ra,  [r1, 44]

  # use r9 as scratch
  mov  r9, flg
  swa  r9,  [r1, 48]

  mov  r9, psr
  swa  r9,  [r1, 52]

  mov  r9, pid
  swa  r9,  [r1, 60]

  mov  r9, tlba
  swa  r9,  [r1, 64]

  mov  r9, tlbf
  swa  r9,  [r1, 68]

  crmv r9, ksp
  swa  r9,  [r1, 72]

  # TCB offset 56 stores per-thread IMR state
  # The caller passes the outgoing thread's pre-switch IMR in r6 ("was")
  swa  r6,  [r1, 56]

  lwa  r9, [r2, 52] # load new thread's PSR
  mov  psr, r9

  # restore old state
  lwa  r20, [r2, 0]
  lwa  r21, [r2, 4]
  lwa  r22, [r2, 8]
  lwa  r23, [r2, 12]
  lwa  r24, [r2, 16]
  lwa  r25, [r2, 20]
  lwa  r26, [r2, 24]
  lwa  r27, [r2, 28]
  lwa  r28, [r2, 32]

  lwa  bp,  [r2, 40]
  lwa  r9, [r2, 36]
  crmv sp, r9

  # use r10 as scratch
  lwa  r10, [r2, 48]
  mov  flg, r10

  lwa  r10, [r2, 60]
  mov  pid, r10

  lwa  r10, [r2, 72]
  crmv ksp, r10

  tlbc # clear tlb

  lwa  r10, [r2, 64]
  mov  tlba, r10

  lwa  r10, [r2, 68]
  mov  tlbf, r10

  lwa  r10, [r2, 44] # r10 holds our return address
  push r10

  # update current thread
  swa  r2,  [r5]

  # check if we should restore interrupts before the callback
  cmp  r7, r0
  bz   context_switch_no_interrupts

	# restore interrupt flags
  lwa  r9,  [r2, 56]
  mov  imr, r9

context_switch_no_interrupts:
  push r2 # save/restore r2 across the callback so we can still read the TCB after it returns

  # call the function we were passed, after the switch
  mov  r1, r4
  bra  ra, r3

  pop r2 

  # restore interrupts after callback regardless of run_with_interrupts
  # since either way we want interrupts restored at this point
  lwa  r9,  [r2, 56]
  mov  imr, r9

  pop  ra

	ret
