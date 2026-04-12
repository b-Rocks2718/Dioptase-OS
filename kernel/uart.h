#ifndef UART_H
#define UART_H

void uart_init(void);

extern void uart_rx_handler_(void);
extern void mark_uart_rx_handled(void);

#endif // UART_H