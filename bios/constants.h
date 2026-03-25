#ifndef CONSTANTS_H
#define CONSTANTS_H

// Common BIOS constants shared by BIOS C and assembly sources

#define NULL 0

#define bool int

#define true 1
#define false 0

// Current maximum supported core count in emulator/FPGA builds.
#define MAX_CORES 4

#define UINT_MAX 0xFFFFFFFF

// Per-thread stack size in bytes for code that reuses kernel thread helpers.
#define TCB_STACK_SIZE 16384 // 16KiB

// The tilemap reserves 256 glyph slots and each glyph is an 8x8 = 64-pixel tile.
#define TILEMAP_PIXELS 16384

// Text mode writes into an 80x60 tile grid.
#define TILE_ROW_WIDTH 80

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

// Pixel framebuffer resolution in pixels.
#define FB_WIDTH 320
#define FB_HEIGHT 240

// Tile index reserved by load_text_tiles() for the transparent glyph.
#define TRANSPARENT 255

#define SD_BLOCK_SIZE_BYTES 512

#endif // CONSTANTS_H
