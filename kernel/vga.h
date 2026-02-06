#ifndef VGA_H
#define VGA_H

extern short* TILEMAP;
extern short* TILE_FB;
extern short* TILE_VSCROLL;

extern char* TILE_SCALE;
extern char* PIXEL_SCALE;

extern short* PIXEL_FB;

void make_tiles_transparent(void);

#endif // VGA_H