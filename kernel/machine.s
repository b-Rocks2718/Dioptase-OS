  
  .text
  .align 4

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

  # get the core's current process id (cr1)
  .global get_pid
get_pid:
  mov r1, pid
  ret

  # set the core's current process id (cr1) to the value in r1
  # returns the old pid in r1
  .global set_pid
set_pid:
  mov r2,  pid
  mov pid, r1
  mov r1, r2
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
  mov r1, tlba
  ret

  # Read the TLB entry for the virtual address in r1, and put the result in r1
  .global tlb_read
tlb_read:
  tlbr r1, r1
  ret

  # Write the TLB entry for the virtual address in r1, with the value in r2
  .global tlb_write
tlb_write:
  tlbw r2, r1
  ret

  # Invalidate the TLB entry for the virtual address in r1
  .global tlb_invalidate
tlb_invalidate:
  tlbi r1
  ret

  .global tlb_invalidate_other
tlb_invalidate_other:
  # invalidate the TLB entry for the virtual address in r2 on the core with the pid in r1
  mov  r3, pid
  mov  pid, r1
  tlbi r2
  mov  pid, r3
  ret

  # invalidate all tlb entries on this core
  .global tlb_flush
tlb_flush:
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
