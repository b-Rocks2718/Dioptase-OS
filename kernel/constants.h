#ifndef CONSTANTS_H
#define CONSTANTS_H

// common constants used throughout the kernel

// General C constants
#define NULL 0

#define bool int

#define true 1
#define false 0

#define UINT_MAX 0xFFFFFFFF

// Hardware constants

#define MAX_CORES 4

// 256 * 64
#define TILEMAP_PIXELS 16384

#define TILE_ROW_WIDTH 80
#define TILE_COL_HEIGHT 60

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

#define FB_WIDTH 320
#define FB_HEIGHT 240

#define TRANSPARENT 255

#define GLOBAL_INT_ENABLE 0x80000000
#define PIT_INT_ENABLE 0x1
#define PS2_INT_ENABLE 0x2
#define UART_RX_INT_ENABLE 0x4
#define SD_0_INT_ENABLE 0x8
#define VGA_VBLANK_INT_ENABLE 0x10
#define IPI_INT_ENABLE 0x20
#define SD_1_INT_ENABLE 0x40

// Kernel constants

#define TCB_STACK_SIZE 16384 // 16KiB

#endif // CONSTANTS_H
