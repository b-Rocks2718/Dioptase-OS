#include "vga.h"

#include "constants.h"

short* TILEMAP = (short*)0x7FE8000;
short* TILE_FB = (short*)0x7FBD000;
short* TILE_VSCROLL = (short*)0x7FE5B42;
char* TILE_SCALE = (char*)0x7FE5B44;
char* PIXEL_SCALE = (char*)0x7FE5B54;

short* PIXEL_FB = (short*)0x7FC0000;

void make_tiles_transparent(void){
  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = TRANSPARENT;
  }
}
