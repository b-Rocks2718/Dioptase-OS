#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "constants.h"

#define GLOBAL_INT_ENABLE 0x80000000
#define PIT_INT_ENABLE 0x1
#define PS2_INT_ENABLE 0x2
#define UART_RX_INT_ENABLE 0x4
#define SD_0_INT_ENABLE 0x8
#define VGA_VBLANK_INT_ENABLE 0x10
#define IPI_INT_ENABLE 0x20
#define SD_1_INT_ENABLE 0x40

// disables interrupts and returns previous imr value
extern unsigned interrupts_disable(void);

// restores imr to prev, returns value that was in imr
extern unsigned interrupts_restore(unsigned prev);

#endif // INTERRUPTS_H
