#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "config.h"
#include "debug.h"
#include "vga.h"

struct PreemptSpinLock print_lock = { 0 };

char* UART_PADDR = (char*)0x7FE5802;

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10      // ABI: int is 4 bytes, so the largest decimal magnitude has 10 digits.
#define MAX_UNSIGNED_HEX_DIGITS 8  // ABI: unsigned is 4 bytes, so hexadecimal output needs at most 8 digits.

int text_color = 0xFF; // Note: access only when holding print_lock

static int vga_index = 0;
bool scrolling = false;
static bool was_newline = false;

// print a single character to the console
void putchar(char c){
  putchar_color(c, text_color);
}

// print a character with a specific color
// color is in the format 0bRRRGGGBB
// scrolls the screen if we run out of room
void putchar_color(char c, int color){
  if (CONFIG.use_vga){

    if (was_newline && scrolling){
      // clear the new line we're about to write on if we just scrolled
      for (int i = 0; i < TILE_ROW_WIDTH; ++i){
        TILE_FB[vga_index + i] = 0;
      }
      // scroll up by one line
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
      // screen is full, scroll up by one line
      vga_index -= FB_NUM_TILES;
      scrolling = true;
    }
  } else {
    // just write to UART, ignoring color
    *UART_PADDR = c;
  }
}

bool isnum(char c){
  return ('0' <= c && c <= '9');
}

// print a string
unsigned puts(char* str){
  unsigned count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
unsigned say(char* fmt, void* arr){
  preempt_spin_lock_acquire(&print_lock);
  unsigned count = printf(fmt, arr);
  preempt_spin_lock_release(&print_lock);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output and allows specifying text color
unsigned say_color(char* fmt, void* arr, int color){
  preempt_spin_lock_acquire(&print_lock);
  int old_color = text_color;
  text_color = color;
  unsigned count = printf(fmt, arr);
  text_color = old_color;
  preempt_spin_lock_release(&print_lock);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// does not acquire print_lock, 
// so output can be interleaved if other threads are printing
// to avoid this, call say() instead of printf()
unsigned printf(char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed(((int*)arr)[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned(((unsigned*)arr)[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex(((unsigned*)arr)[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex(((unsigned*)arr)[i++], true);
      } else if (*(fmt + 1) == 's') {
        ++fmt;
        count += puts((char*)((void**)arr)[i++]);
      } else if (*(fmt + 1) == '%') {
        ++fmt;
        putchar('%');
        ++count;
      } else {
        // unsupported format specifier, print as is
        putchar(*fmt);
        ++count;
      }
    } else {
      putchar(*fmt);
      ++count;
    }
    ++fmt;
  }
  return count;
}

// print the number n to the console as a signed decimal
// returns the number of characters printed
unsigned print_signed(int n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned magnitude;
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  if(n < 0){
    putchar('-');
    magnitude = 0u - (unsigned)n;
  } else {
    magnitude = (unsigned)n;
  }

  while (magnitude != 0){
    digits[len++] = (char)('0' + (magnitude % DECIMAL_BASE));
    magnitude /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return (n < 0) ? (count + 1) : count;
}

// print the number n to the console as an unsigned decimal
// returns the number of characters printed
unsigned print_unsigned(unsigned n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    digits[len++] = (char)('0' + (n % DECIMAL_BASE));
    n /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// print the number n to the console as a hexadecimal
// returns the number of characters printed
unsigned print_hex(unsigned n, bool uppercase){
  char digits[MAX_UNSIGNED_HEX_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    unsigned digit = n % HEX_BASE;

    if (digit < DECIMAL_BASE){
      digits[len++] = (char)('0' + digit);
    } else {
      digits[len++] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
    }

    n /= HEX_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// load text mode tiles and initialize VGA text mode
void vga_text_init(void){
  if (CONFIG.use_vga){
    load_text_tiles(); // text tileset is included in kernel image

    *TILE_SCALE = 0;
    *TILE_VSCROLL = 0;
  }
}

// clear the screen of all text characters and reset scroll/cursor state
void clear_screen(void){
  preempt_spin_lock_acquire(&print_lock);

  vga_index = 0;
  scrolling = false;
  was_newline = false;

  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = 0;
  }

  preempt_spin_lock_release(&print_lock);
}

// set the current tileset to the text mode tileset and clear the screen
void load_text_tiles(void){
  // text_tiles is a list of addresses that we need to store 0xC000 at
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
 
// set the current tileset to the text mode tileset with the given text and background colors, 
// then clear the screen
void load_text_tiles_colored(short text_color, short bg_color){
  // text_tiles is a list of addresses that we need to store text_color at
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = bg_color;
  }

  clear_screen();

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = text_color;
    offset = text_tiles[i++];
  }

  // transparent tile at index 255
  for (int i = 0; i < 64; ++i){
    // 16320 = 255 * 64
    TILEMAP[16320 + i] = 0xF000;
  }
}
