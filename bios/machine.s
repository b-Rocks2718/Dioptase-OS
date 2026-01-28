  
.define UART_PADDR 0x7FE5802

  .global putchar
putchar:
  # write character in r1 to UART
  movi r2, UART_PADDR
  sba r1, [r2]
  ret

  .global get_cr0
get_cr0:
  mov r1, cr0
  ret

  .global enter_kernel
enter_kernel:
  # Load the kernel start address from r1 and jump to it
  jmp r1
  mode halt # Should never return
