  
  .align 4
  .text

  # Permanantly halt this core
  # in the emulator, and core running this will end the emulation
  .global shutdown
shutdown:
  mode halt
  ret

  # Put this core to sleep until an interrupt wakes it up
  # Ensure interrupts are enabled before sleeping, or the core may never wake up
  .global mode_sleep
mode_sleep:
  mode sleep
  ret

  # return the core's id (0 - 3)
  # read from the cores cr9 register, which is read-only
  .global get_core_id
get_core_id:
  mov r1, cid
  ret

  # return the core's cr0 value, which determines whether we're in user mode or kernel mode
  # 0 = user mode, nonzero = kernel mode
  # cr0 is a counter that is incremented on syscalls/exceptions/interrupts, and decremented when they exit
  .global get_cr0
get_cr0:
  mov r1, cr0
  ret
  
  # Return the core's interrupt status register (cr2) value
  .global get_isr
get_isr:
  mov r1, isr
  ret

  # Return the current interrupt mask register (cr3) value
  .global get_imr
get_imr:
  mov r1, imr
  ret

  # Return the exception program counter (cr4) value
  .global get_epc
get_epc:
  mov r1, epc
  ret

  # Return the exception flags (cr6) value
  .global get_efg
get_efg:
  mov r1, efg
  ret

  # Return the TLB miss address register (cr7) value
  .global get_tlb_addr
get_tlb_addr:
  mov r1, tlb
  ret

  .global get_return_address
get_return_address:
  # Purpose: return the caller address using the current frame pointer.
  # Inputs: none.
  # Outputs: r1 = return address of the current function's caller.
  # Preconditions: ABI prologue has saved ra at [bp, 4].
  # Postconditions: no state changes besides r1.
  # CPU state assumptions: kernel mode; interrupts may be enabled.
  lwa  r1, [bp, 4]
  ret

  .global get_caller_return_address
get_caller_return_address:
  # Purpose: return the caller's caller address using chained frame pointers.
  # Inputs: none.
  # Outputs: r1 = return address two frames up (caller of caller).
  # Preconditions: ABI prologues have saved bp/ra for the two callers.
  # Postconditions: no state changes besides r1.
  # CPU state assumptions: kernel mode; interrupts may be enabled.
  mov  r1, bp
  lwa  r1, [r1]     # caller bp
  lwa  r1, [r1]     # caller's caller bp
  lwa  r1, [r1, 4]  # caller's caller ra
  ret

  .global __atomic_exchange_n
__atomic_exchange_n:
  # atomic exchange: swap value in r1 with value in r2
  # put old value in r1
  swpa r1, r2, [r1]
  ret

  .global __atomic_fetch_add
__atomic_fetch_add:
  # atomic fetch add: add value in r2 to value stored at r1
  # put old value in r1
  fada r1, r2, [r1]
  ret

  .global __atomic_load_n
__atomic_load_n:
  # atomic load: load value stored at r1 into r1
  lwa r1, [r1]
  ret

  .global __atomic_store_n
__atomic_store_n:
  # atomic store: store value in r2 to address in r1
  swa r2, [r1]
  ret

  .global wakeup_core
wakeup_core:
  # wake up other cores based on number in r1 by sending an IPI
  # puts success in r1
  cmp r1, 4
  bbe wakeup_core_error
  cmp r1, 3
  bz  wakeup_core_3
  cmp r1, 2
  bz  wakeup_core_2
  cmp r1, 1
  bz  wakeup_core_1
  cmp r1, 0
  bz  wakeup_core_0
  jmp wakeup_core_error
wakeup_core_0:
  ipi r1, 0
  ret
wakeup_core_1:
  ipi r1, 1
  ret
wakeup_core_2:
  ipi r1, 2
  ret
wakeup_core_3:
  ipi r1, 3
  ret
wakeup_core_error:
  movi r1, 0xEEEE
  mode halt

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
  # call the function we were passed, after the switch
  push r2

  mov  r1, r4
  bra  ra, r3

  pop r2

  # restore interrupts after callback regardless of run_with_interrupts
  # since either way we want interrupts restored at this point
  lwa  r9,  [r2, 132]
  mov  imr, r9

  pop  ra

	ret
