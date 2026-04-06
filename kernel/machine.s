  
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

  # set the core's current process id (cr1) to the value in r1
  .global set_pid
set_pid:
  mov cr1, r1
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

  # invalidate all tlb entries on this core
  .global flush_tlb
flush_tlb:
  tlbc
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
