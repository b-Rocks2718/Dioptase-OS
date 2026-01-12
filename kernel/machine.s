
.global halt
halt:
  mode halt
  

.define UART_PADDR 0x7FE5802

  .global putchar
putchar:
  # write character in r1 to UART
  movi r2, UART_PADDR
  sba r1, [r2]
  ret