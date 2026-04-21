  .text
  .align 4

  .global trap_handler_
trap_handler_:
  # Trap ABI on entry:
  # - r1 holds the user-selected trap code.
  # - r2-r8 hold trap-specific arguments.

  # Trap entry already cleared IMR[31]. Snapshot the interrupted PC/flags
  # before re-enabling nested interrupts so rfe later restores the exact trap
  # entry state.
  mov  r11, epc # r9, r10, r11 are caller saved and do not contain any arguments
  push r11
  mov  r9, efg
  push r9

  # Re-enable interrupts after the trap frame is fully saved
  movi r9, 0x80000000
  mov  r10, imr
  or   r10, r9, r10
  mov  imr, r10

  push r0 # allocate stack space for the return_to_user boolean
  mov  r12, sp # preserve the boolean slot address before pushing stack args

  crmv r9, sp
  push r9  # pass user sp to trap_handler
  push r11 # pass user pc to trap_handler

  push r12 # pass the address of the return_to_user boolean to trap_handler

  call trap_handler

  # get return_to_user boolean from the stack
  add  sp, sp, 12 # remove the arguments pushed for trap_handler
  pop  r2 # load return_to_user boolean into r2

  cmp  r2, r0
  bz   return_to_kernel

  # Disable the global interrupts again before restoring EPC/EFG and
  # returning through rfe
  movi r9, 0x7FFFFFFF
  mov  r10, imr
  and  r10, r9, r10
  mov  imr, r10

  pop  r9
  mov  efg, r9
  pop  r9
  mov  epc, r9

  rfe

return_to_kernel:
  add sp, sp, 8 # 2 regs

  pop ra
  pop bp

  ret

  .global jump_to_user
jump_to_user:
  # disable interrupts before setting up epc and efg
  movi r9, 0x7FFFFFFF
  mov  r10, imr
  and  r10, r9, r10
  mov  imr, r10
  # interrupts will be restored on rfe

  # save bp and ra for when the user program exits
  push bp
  push ra

  # set up user stack
  crmv sp, r2

  # set up user program counter
  mov epc, r1

  # clear flags
  mov efg, r0

  # set up user registers r1 and r2 with the values passed in
  mov r1, r3
  mov r2, r4

  rfe

  .global copy_user
  # int copy_user(void* dest, void* src, unsigned n, struct TCB* cur_tcb);
copy_user:
  # set up fail addr
  adpc r5, copy_user_fail
  swa  r5, [r4, 80]

  # mark a user access in progress
  movi r5, 1
  swa  r5, [r4, 76]

copy_user_loop:
  cmp  r3, r0
  bz   copy_user_done
  lba  r5, [r2], 1
  sba  r5, [r1], 1
  add  r3, r3, -1
  jmp  copy_user_loop

copy_user_done:
  # clear user access in progress
  movi r5, 0
  swa  r5, [r4, 76]

  # clear the fail address
  movi r5, 0
  swa  r5, [r4, 80]

  # return 0
  mov  r1, r0

  ret

copy_user_fail:
  # clear user access in progress
  movi r5, 0
  swa  r5, [r4, 76]

  # clear the fail address
  movi r5, 0
  swa  r5, [r4, 80]

  # return -1
  movi r1, -1

  ret
