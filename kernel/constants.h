#ifndef CONSTANTS_H
#define CONSTANTS_H

#define NULL 0

#define bool int

#define true 1
#define false 0

#define MAX_CORES 4

#define UINT_MAX 0xFFFFFFFF

// in bytes
#define TCB_STACK_SIZE 16384 // 16KiB

// 256 * 64
#define TILEMAP_PIXELS 16384

#define TILE_ROW_WIDTH 80

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

#define FB_WIDTH 320
#define FB_HEIGHT 240

#define TRANSPARENT 255

#endif // CONSTANTS_H