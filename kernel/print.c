#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "config.h"

struct SpinLock print_lock = { 0 };

char* UART_PADDR = (char*)0x7FE5802;

short* TILEMAP = (short*)0x7FE8000;
short* TILE_FB = (short*)0x7FBD000;
char* TILE_SCALE = (char*)0x7FE5B44;

int text_color = 0xFF; // Note: use __atomic_exchange_n to avoid races

void putchar(char c){
  static int vga_index = 0;
  if (CONFIG.use_vga){
    if (c == '\n'){
      vga_index++;
      // round up to next row
      vga_index = ((vga_index + TILE_ROW_WIDTH - 1) / TILE_ROW_WIDTH) * TILE_ROW_WIDTH;
    } else {
      TILE_FB[vga_index++] = (short)((text_color << 8) | c);
    }
  } else {
    *UART_PADDR = c;
  }
}

bool isnum(char c){
  return ('0' <= c && c <= '9');
}

unsigned puts(char* str){
  unsigned count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

unsigned say(char* fmt, int* arr){
  spin_lock_acquire(&print_lock);
  unsigned count = printf(fmt, arr);
  spin_lock_release(&print_lock);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X
// accepts an array instead of variadic arguments
unsigned printf(char* fmt, int* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed(arr[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned((unsigned)arr[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex((unsigned)arr[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex((unsigned)arr[i++], true);
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

unsigned print_signed(int n){
  if(n == 0){
      putchar('0');
      return 1;
  }
  if(n < 0){
      putchar('-');
      n = -n;
  }

  unsigned d = n % 10;
  n = n / 10;
  
  unsigned count = 1;

  if (n != 0){
    count += print_signed(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}

unsigned print_unsigned(unsigned n){
  if(n == 0){
      putchar('0');
      return 1;
  }

  unsigned d = n % 10;
  n = n / 10;
  
  unsigned count = 1;

  if (n != 0){
    count += print_unsigned(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}

unsigned print_hex(unsigned n, bool uppercase){
  if(n == 0){
      putchar('0');
      return 1;
  }

  unsigned d = n % 16;
  n = n / 16;
  
  unsigned count = 1;

  if (n != 0){
    count += print_hex(n, uppercase);
  } 
  if (d < 10){
    putchar('0' + d);
  } else {
    putchar((uppercase ? 'A' : 'a') + (d - 10));
  }
  
  // return number of characters printed
  return count;
}

void vga_text_init(void){
  if (CONFIG.use_vga){
    load_text_tiles();

    *TILE_SCALE = 0;
  }
}

// text_tiles is a list of addresses that we need to store 0xC000 at
void load_text_tiles(void){
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = 0;
  }

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = 0xC000;
    offset = text_tiles[i++];
  }
}
