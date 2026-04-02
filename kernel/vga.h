#ifndef VGA_H
#define VGA_H

// MMIO addresses for VGA text mode

// 256 * 64
#define TILEMAP_PIXELS 16384

#define TILE_ROW_WIDTH 80
#define TILE_COL_HEIGHT 60

#define FB_NUM_TILES 4800
#define FB_NUM_PIXELS 76800

#define FB_WIDTH 320
#define FB_HEIGHT 240

#define TRANSPARENT 255

extern short* TILEMAP;
extern short* TILE_FB;
extern short* TILE_VSCROLL;

extern char* TILE_SCALE;
extern char* PIXEL_SCALE;

extern short* PIXEL_FB;

// write a transparent tile to every tile in the framebuffer
void make_tiles_transparent(void);

#endif // VGA_H