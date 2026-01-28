  
.define UART_PADDR 0x7FE5802

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
