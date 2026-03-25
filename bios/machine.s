  # BIOS machine helpers used during early boot before kernel_entry

  .align 4
  .text

  .global get_cr0
get_cr0:
  # Return the processor status register depth (cr0 / PSR)
  mov r1, cr0
  ret

  .global enter_kernel
enter_kernel:
  # Jump directly to the loaded kernel entry point
  jmp r1
  mode halt
