#include "uart.h"
#include "ivt.h"

// Initialize the UART by registering the RX interrupt handler
void uart_init(void){
  register_handler((void*)uart_rx_handler_, (void*)UART_RX_IVT_ENTRY);
}

void uart_rx_handler(void){
  // UART RX remains masked by default and has no line discipline yet. If a
  // caller enables the source anyway, acknowledge the edge and return so the
  // machine cannot panic solely from enabling a currently unused interrupt.
  mark_uart_rx_handled();
}
