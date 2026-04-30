#include "../crt/vga.h"
#include "../crt/sys.h"
#include "../crt/print.h"

#include "fix32.h"

#define RESOLUTION 0

#define COLOR_COUNT 56

short* TILE_FB = NULL;

// Count Mandelbrot iterations for one complex coordinate.
int mandelbrot_count(struct Complex* c){
  struct Complex z = {0, 0};
  int i;
  for (i = 0; i < COLOR_COUNT; ++i){
    struct Complex temp;
    mul_complex(&z, &z, &temp);
    add_complex(c, &temp, &z);
    fix32 d = norm(&z);
    if (d > 4 * FIXED_ONE) break;
  }
  if (i == COLOR_COUNT) i = -1;
  return i;
}

// Render one Mandelbrot view into the tile framebuffer.
void display_mandelbrot(fix32 start_x, fix32 start_y, fix32 diff){
  for (int i = 0; i < TILE_COL_HEIGHT >> RESOLUTION; ++i){
    for (int j = 0; j < TILE_ROW_WIDTH >> RESOLUTION; ++j){
      struct Complex c = {start_x + diff * j, start_y - diff * i};
      int count = mandelbrot_count(&c);
      if (count >= 0){
        TILE_FB[i * TILE_ROW_WIDTH + j] = (0x1C00) | (33 + count);
      } else {
        TILE_FB[i * TILE_ROW_WIDTH + j] = (0x1C00) | 32;
      }
    }
  }
}

// Configure the tile framebuffer view, render it, and wait for q to exit.
int main(void){
  TILE_FB = get_tile_fb();
  set_tile_scale(RESOLUTION);

  // clear screen
  puts("\x1b[2J");

  // hide cursor
  puts("\x1b[?25l");

  // wait a bit for the display to be ready
  sleep(3);

  fix32 start_x = -0x00028000;
  fix32 start_y = 0x00012000;

  fix32 diff = 0x00000266 << (RESOLUTION + 2);

  // Draw one static Mandelbrot view for manual inspection.
  display_mandelbrot(start_x, start_y, diff);

  while (getkey() != 'q'){
    sleep(5);
  }

  // show cursor
  puts("\x1b[?25h");

  // clear screen before exit
  puts("\x1b[2J");

  // home cursor
  puts("\x1b[H");

  // restore tile scale
  set_tile_scale(0);

  return 0;
}
