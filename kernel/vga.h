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

// The part of the pixel framebuffer a user mapping may expose. The framebuffer
// is 320x240 16-bit pixels at 0x7FC0000..0x7FE57FF (docs/mem_map.md); its 37
// whole pages are 0x7FC0000..0x7FE4FFF, pixels 0..75775. The final 2,048
// framebuffer bytes share page 0x7FE5000 with the PS/2, UART, PIT, SD DMA,
// audio, and sprite/scroll registers, and mappings are page-granular, so
// mapping that page would hand user code device control (an SD DMA into
// kernel memory, a reprogrammed PIT). Pixels 256..319 of row 236 and rows
// 237..239 therefore stay kernel-only.
#define PIXEL_FB_USER_SIZE 0x25000
#define PIXEL_FB_USER_PIXELS (PIXEL_FB_USER_SIZE / 2)  // 75776
#define PIXEL_FB_PIXELS (320 * 240)                   // the whole layer

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
// Pixel-layer scroll, signed 16-bit pixels; the layer wraps at 320x240.
extern volatile short * const PIXEL_HSCROLL;
extern volatile short * const PIXEL_VSCROLL;

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
