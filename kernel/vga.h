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

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

#define FB_WIDTH 320
#define FB_HEIGHT 240

#define TRANSPARENT 255

extern short* TILEMAP;
extern short* TILE_FB;
extern short* TILE_HSCROLL;
extern short* TILE_VSCROLL;
extern char* VGA_STATUS;
extern unsigned* VGA_FRAME_COUNTER;

extern char* TILE_SCALE;
extern char* PIXEL_SCALE;

extern short* PIXEL_FB;

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
