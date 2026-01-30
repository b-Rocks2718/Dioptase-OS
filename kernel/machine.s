  
.define UART_PADDR 0x7FE5802
  .align 4
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

  .global sleep
sleep:
  mode sleep
  ret

  .global context_switch # (struct TCB* me, struct TCB* next, void (*func)(void *), void *arg)
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

  swa  sp,  [r1, 32]
  swa  bp,  [r1, 36]

  # use r9 as scratch
  mov  r9, flg
  swa  r9,  [r1, 40]

  swa  ra,  [r1, 44]

  mov  r9, imr # save interrupt flags in r9
  mov  imr, r0 # temporarily disable interrupts

  # restore old state
  lwa  r20, [r2, 0]
  lwa  r21, [r2, 4]
  lwa  r22, [r2, 8]
  lwa  r23, [r2, 12]
  lwa  r24, [r2, 16]
  lwa  r25, [r2, 20]
  lwa  r26, [r2, 24]
  lwa  r27, [r2, 28]

  lwa  sp,  [r2, 32]
  lwa  bp,  [r2, 36]

  # use r10 as scratch
  lwa  r10, [r2, 40]
  mov  flg, r10

  lwa  r10, [r2, 44] # r10 holds our return address
  push r10

	# restore interrupt flags
  mov  imr, r9

  # call the function we were passed, after the switch
  mov  r1, r4
  bra  ra, r3

  pop  ra

	ret
