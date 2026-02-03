  
.define UART_PADDR 0x7FE5802
  .align 4
  .text
  .global putchar
putchar:
  # write character in r1 to UART
  movi r2, UART_PADDR
  sba r1, [r2]
  ret

  .global get_core_id
get_core_id:
  mov r1, cid
  ret

  # cr0 holds the core's kernel state (0 => user mode, nonzero => kernel mode)
  .global get_cr0
get_cr0:
  mov r1, cr0
  ret

  .global get_pc
get_pc:
  adpc r1, 0
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

  .global get_isr
get_isr:
  mov r1, isr
  ret

  .global get_imr
get_imr:
  mov r1, imr
  ret

  .global get_epc
get_epc:
  mov r1, epc
  ret

  .global get_efg
get_efg:
  mov r1, efg
  ret

  .global get_tlb_addr
get_tlb_addr:
  mov r1, cr7
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
  # wake up other cores based on number in r1
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

  .global shutdown
shutdown:
  mode halt
  ret

  .global mode_sleep
mode_sleep:
  mode sleep
  ret

  .global context_switch # (struct TCB* me, struct TCB* next, void (*func)(void *), void *arg, struct TCB** cur_thread, int was)
context_switch:
  mov  imr, r0 # temporarily disable interrupts

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

  mov  r9, r6 # was
  swa  r9,  [r1, 132]

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

	# restore interrupt flags
  lwa  r9,  [r2, 132]
  mov  imr, r9

  # call the function we were passed, after the switch
  mov  r1, r4
  bra  ra, r3

  pop  ra

	ret
