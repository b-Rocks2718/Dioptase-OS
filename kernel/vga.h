#ifndef VGA_H
#define VGA_H

// MMIO addresses for VGA text mode

// 256 tiles * 64 pixels per 8x8 tile
#define TILEMAP_PIXELS 16384

#define TILE_ROW_WIDTH 80
#define TILE_COL_HEIGHT 60

// docs/mem_map.md defines both tile MMIO regions as 16-bit little-endian data.
#define TILEMAP_SIZE 32768
#define TILE_FB_SIZE 9600

#define SPRITEMAP_SIZE 0x8000
#define NUM_SPRITES 16

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

#define FB_WIDTH 320
#define FB_HEIGHT 240

// User mapping of PIXEL_FB. The hardware buffer is 320*240*2 = 153600 bytes
// (0x7FC0000 through 0x7FE57FF, docs/mem_map.md). mmap_physmem rounds up to
// whole pages, and the next page also contains the PS/2, UART, PIT, SD, and
// audio registers at 0x7FE5800. Map only the page-aligned prefix that ends at
// 0x7FE5000 so a user store cannot reach those devices. Rows 0..235 are
// complete; row 236 is only partially mapped.
#define PIXEL_FB_USER_SIZE 0x25000

#define TRANSPARENT 255

// Display MMIO from docs/mem_map.md. Each pointer is a fixed device address.
// The cells are volatile so every access is a real device operation.
// VGA_STATUS and VGA_FRAME_COUNTER are read-only in hardware.
extern volatile short * const TILEMAP;
extern volatile short * const TILE_FB;
extern volatile short * const SPRITEMAP;
extern volatile short * const TILE_HSCROLL;
extern volatile short * const TILE_VSCROLL;
extern const volatile char * const VGA_STATUS;
extern const volatile unsigned * const VGA_FRAME_COUNTER;

extern volatile char * const TILE_SCALE;
extern volatile char * const PIXEL_SCALE;

extern volatile short * const PIXEL_FB;

extern volatile char * const SPRITE_SCALES;
extern volatile short * const SPRITE_COORDS;

// initialize the VGA hardware and framebuffer
void vga_init(void);

// set the current tileset to the text mode tileset and clear the screen
void load_text_tiles(void);

// set the current tileset to the text mode tileset with the given text and background colors, 
// then clear the screen
void load_text_tiles_colored(short text_color, short bg_color);

// write a transparent tile to every tile in the framebuffer
void make_tiles_transparent(void);

// interrupt entry point, defined in vga.s
extern void vga_vblank_handler_(void);
extern void mark_vblank_handled(void);

#endif // VGA_H
