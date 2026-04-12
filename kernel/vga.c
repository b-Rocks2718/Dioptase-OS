#include "vga.h"

#include "constants.h"
#include "print.h"
#include "ivt.h"
#include "debug.h"

// MMIO addresses for VGA text mode

short* TILEMAP = (short*)0x7FE8000;
short* TILE_FB = (short*)0x7FBD000;
short* TILE_HSCROLL = (short*)0x7FE5B40;
short* TILE_VSCROLL = (short*)0x7FE5B42;
char* VGA_STATUS = (char*)0x7FE5B46;
unsigned* VGA_FRAME_COUNTER = (unsigned*)0x7FE5B48;
char* TILE_SCALE = (char*)0x7FE5B44;
char* PIXEL_SCALE = (char*)0x7FE5B54;

short* PIXEL_FB = (short*)0x7FC0000;

void vga_init(void){
  vga_text_init();

  register_handler((void*)vga_vblank_handler_, (void*)VGA_VBLANK_IVT_ENTRY);
}

// write a transparent tile to every tile in the framebuffer
void make_tiles_transparent(void){
  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = TRANSPARENT;
  }
}

void vga_vblank_handler(void){
  mark_vblank_handled();

  panic("| VGA VBLANK handler unexpectedly called\n");
}
