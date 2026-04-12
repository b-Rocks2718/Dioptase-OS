#include "uart.h"
#include "ivt.h"
#include "debug.h"

// Initialize the UART by registering the RX interrupt handler
void uart_init(void){
  register_handler((void*)uart_rx_handler_, (void*)UART_RX_IVT_ENTRY);
}

void uart_rx_handler(void){
  mark_uart_rx_handled();
  panic("| Unexpected UART RX interrupt received\n");
}
