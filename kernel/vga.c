#include "vga.h"

#include "constants.h"
#include "print.h"
#include "ivt.h"

// Display MMIO from docs/mem_map.md. Pointers stay fixed; cells stay volatile.
// Status and the frame counter are hardware read-only.

volatile short * const TILEMAP = (volatile short *)0x7FE8000;
volatile short * const TILE_FB = (volatile short *)0x7FBD000;
volatile short * const SPRITEMAP = (volatile short *)0x7FF0000;
volatile short * const TILE_HSCROLL = (volatile short *)0x7FE5B40;
volatile short * const TILE_VSCROLL = (volatile short *)0x7FE5B42;
const volatile char * const VGA_STATUS = (const volatile char *)0x7FE5B46;
const volatile unsigned * const VGA_FRAME_COUNTER = (const volatile unsigned *)0x7FE5B48;
volatile char * const TILE_SCALE = (volatile char *)0x7FE5B44;
volatile char * const PIXEL_SCALE = (volatile char *)0x7FE5B54;

volatile short * const PIXEL_FB = (volatile short *)0x7FC0000;

volatile char * const SPRITE_SCALES = (volatile char *)0x7FE5B60;
volatile short * const SPRITE_COORDS = (volatile short *)0x7FE5B00;

// Initialize VGA registers and the text-mode tile state.
void vga_init(void){
  vga_text_init();

  register_handler((void*)vga_vblank_handler_, (void*)VGA_VBLANK_IVT_ENTRY);
}

// write a transparent tile to every tile in the framebuffer
void make_tiles_transparent(void){
  // Console output and this pixel-layer handoff share TILE_FB across all cores.
  // The console bulk transaction keeps the long MMIO loop interruptible while
  // excluding other cores and diverting same-core nested diagnostics to UART.
  console_make_tiles_transparent();
}

// Service the vertical-blank interrupt and publish a completed frame.
void vga_vblank_handler(void){
  // VBLANK remains masked by default and has no display-refresh consumer yet.
  // If a caller enables the source anyway, acknowledge the edge and return so
  // enabling the interrupt cannot escalate into a kernel panic by itself.
  mark_vblank_handled();
}
