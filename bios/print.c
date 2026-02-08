#include "print.h"
#include "machine.h"
#include "config.h"
#include "constants.h"

char* UART_PADDR = (char*)0x7FE5802;

short* TILEMAP = (short*)0x7FE8000;
short* TILE_FB = (short*)0x7FBD000;
short* TILE_VSCROLL = (short*)0x7FE5B42;
char* TILE_SCALE = (char*)0x7FE5B44;
char* PIXEL_SCALE = (char*)0x7FE5B54;

short* PIXEL_FB = (short*)0x7FC0000;

int text_color = 0xFF; // Note: access only when holding print_lock

static int vga_index = 0;
bool scrolling = false;
static bool was_newline = false;

void putchar(char c){
  putchar_color(c, text_color);
}

void putchar_color(char c, int color){
  if (CONFIG.use_vga){

    if (was_newline && scrolling){
      for (int i = 0; i < TILE_ROW_WIDTH; ++i){
        TILE_FB[vga_index + i] = 0;
      }
      *TILE_VSCROLL = *TILE_VSCROLL - 8;
      was_newline = false;
    }

    if (c == '\n'){
      vga_index++;
      // round up to next row
      vga_index = ((vga_index + TILE_ROW_WIDTH - 1) / TILE_ROW_WIDTH) * TILE_ROW_WIDTH;

      was_newline = true;
    } else {
      TILE_FB[vga_index++] = (short)((color << 8) | c);
    }

    if (vga_index >= FB_NUM_TILES) {
      vga_index -= FB_NUM_TILES;
      scrolling = true;
    }
  } else {
    *UART_PADDR = c;
  }
}

int puts(char* str){
  int count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

int print_num(int n){
  if(n == 0){
      putchar('0');
      return 1;
  }
  if(n < 0){
      putchar('-');
      n = -n;
  }

  int d = n % 10;
  n = n / 10;
  
  int count = 1;

  if (n != 0){
    count += print_num(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}

void vga_text_init(void){
  if (CONFIG.use_vga){
    load_text_tiles();

    *TILE_SCALE = 0;
    *TILE_VSCROLL = 0;
  }
}

void clear_screen(void){
  vga_index = 0;
  scrolling = false;
  was_newline = false;

  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = 0;
  }
}

// text_tiles is a list of addresses that we need to store 0xC000 at
void load_text_tiles(void){
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = 0;
  }

  clear_screen();

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = 0xC000;
    offset = text_tiles[i++];
  }

  // transparent tile at index 255
  for (int i = 0; i < 64; ++i){
    // 16320 = 255 * 64
    TILEMAP[16320 + i] = 0xF000;
  }
}
