#include "../kernel/machine.h"
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/threads.h"
#include "../kernel/pit.h"
#include "../kernel/config.h"
#include "../kernel/ps2.h"
#include "../kernel/debug.h"
#include "../kernel/vga.h"

#define fix32 int

#define FIXED_ONE 0x00010000

#define RESOLUTION 0

struct Complex {
  fix32 x;
  fix32 y;
};

fix32 mul_fixed(fix32 a, fix32 b){
  unsigned ua = (unsigned)a;
  unsigned ub = (unsigned)b;
  int a_hi = a >> 16;
  int b_hi = b >> 16;
  unsigned a_lo = ua & 0xFFFF;
  unsigned b_lo = ub & 0xFFFF;

  // Q16.16 multiply using 32-bit partial products only.
  unsigned hi = ((unsigned)(a_hi * b_hi)) << 16;
  unsigned mid = (unsigned)(a_hi * (int)b_lo) +
                 (unsigned)(b_hi * (int)a_lo);
  unsigned lo = (a_lo * b_lo) >> 16;
  return (fix32)(hi + mid + lo);
}

void add_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out){
  out->x = a->x + b->x;
  out->y = a->y + b->y;
}

void mul_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out){
  out->x = mul_fixed(a->x, b->x) - mul_fixed(a->y, b->y);
  out->y = mul_fixed(a->x, b->y) + mul_fixed(a->y, b->x);
}

fix32 norm(struct Complex* z){
  return mul_fixed(z->x, z->x) + mul_fixed(z->y, z->y);
}

#define COLOR_COUNT 56

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

int kernel_main(void){
  *PIXEL_SCALE = RESOLUTION;

  fix32 start_x = -0x00028000;
  fix32 start_y = 0x00012000;

  fix32 diff = 0x00000266 << (RESOLUTION + 2);

  display_mandelbrot(start_x, start_y, diff);

  while (getkey() != 'q');

  return 0;
}
