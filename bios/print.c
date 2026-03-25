#include "print.h"
#include "machine.h"
#include "config.h"
#include "constants.h"

// Console MMIO addresses from docs/mem_map.md.
#define UART_TX_PADDR ((char*)0x7FE5802)
#define TILEMAP_PADDR ((short*)0x7FE8000)
#define TILE_FB_PADDR ((short*)0x7FBD000)
#define TILE_VSCROLL_PADDR ((short*)0x7FE5B42)
#define TILE_SCALE_PADDR ((char*)0x7FE5B44)
#define PIXEL_SCALE_PADDR ((char*)0x7FE5B54)
#define PIXEL_FB_PADDR ((short*)0x7FC0000)

#define TILE_HEIGHT_PIXELS 8
#define TILE_PIXELS_PER_GLYPH 64
#define TILE_PIXEL_FOLLOWS_FB_COLOR 0xC000
#define TILE_PIXEL_TRANSPARENT 0xF000
#define BIOS_DEFAULT_TEXT_COLOR 0xFF

char* UART_PADDR = UART_TX_PADDR;

short* TILEMAP = TILEMAP_PADDR;
short* TILE_FB = TILE_FB_PADDR;
short* TILE_VSCROLL = TILE_VSCROLL_PADDR;
char* TILE_SCALE = TILE_SCALE_PADDR;
char* PIXEL_SCALE = PIXEL_SCALE_PADDR;

// Reserved for future BIOS pixel-mode rendering paths.
short* PIXEL_FB = PIXEL_FB_PADDR;

// BIOS console output is single-threaded before kernel handoff, so no lock is
// needed for this shared state.
int text_color = BIOS_DEFAULT_TEXT_COLOR;

static int vga_index = 0;
bool scrolling = false;
static bool was_newline = false;

// print a single character to the BIOS console
void putchar(char c){
  putchar_color(c, text_color);
}

// print a character with a specific color
// color is encoded as 0bRRRGGGBB in the tile framebuffer upper byte
// scrolls the tile layer when the cursor wraps past the bottom of the screen
void putchar_color(char c, int color){
  if (CONFIG.use_vga){

    if (was_newline && scrolling){
      // Clear the row we are about to reuse after one full-screen wrap.
      for (int i = 0; i < TILE_ROW_WIDTH; ++i){
        TILE_FB[vga_index + i] = 0;
      }
      // Scroll the tile layer up by one 8-pixel text row.
      *TILE_VSCROLL = *TILE_VSCROLL - TILE_HEIGHT_PIXELS;
      was_newline = false;
    }

    if (c == '\n'){
      vga_index++;
      // Round up to the next row so newline leaves any skipped cells blank.
      vga_index = ((vga_index + TILE_ROW_WIDTH - 1) / TILE_ROW_WIDTH) * TILE_ROW_WIDTH;

      was_newline = true;
    } else {
      TILE_FB[vga_index++] = (short)((color << 8) | c);
    }

    if (vga_index >= FB_NUM_TILES) {
      // The next newline will scroll the tile layer and reuse row 0.
      vga_index -= FB_NUM_TILES;
      scrolling = true;
    }
  } else {
    // Headless BIOS output goes to UART only; color is ignored.
    *UART_PADDR = c;
  }
}

// print a string
int puts(char* str){
  int count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

// print the number n to the console as a signed decimal
// returns the number of characters printed
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

// load text mode tiles and initialize VGA text mode
void vga_text_init(void){
  if (CONFIG.use_vga){
    load_text_tiles();

    *TILE_SCALE = 0;
    *TILE_VSCROLL = 0;
  }
}

// clear the screen of all text characters and reset scroll/cursor state
void clear_screen(void){
  vga_index = 0;
  scrolling = false;
  was_newline = false;

  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = 0;
  }
}

// text_tiles is a NUL-terminated list of tilemap offsets that should receive
// the "use tile color from framebuffer" sentinel pixel value.
void load_text_tiles(void){
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = 0;
  }

  clear_screen();

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = TILE_PIXEL_FOLLOWS_FB_COLOR;
    offset = text_tiles[i++];
  }

  // Reserve tile 255 as a transparent glyph so the text renderer can erase
  // cells by writing that tile index into the tile framebuffer.
  for (int i = 0; i < TILE_PIXELS_PER_GLYPH; ++i){
    TILEMAP[(TRANSPARENT * TILE_PIXELS_PER_GLYPH) + i] = TILE_PIXEL_TRANSPARENT;
  }
}
