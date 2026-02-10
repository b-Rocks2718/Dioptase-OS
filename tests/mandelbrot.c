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

#define RESOLUTION 3

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

unsigned colors[COLOR_COUNT] =
  {0x44F, 0x45F, 0x46F, 0x47F, 0x48F, 0x49F, 0x4AF, 0x4BF,
   0x4CF, 0x4DF, 0x4EF, 0x4FF, 0x4FF, 0x4FE, 0x4FD, 0x4FC,
   0x4FB, 0x4FA, 0x4F9, 0x4F8, 0x4F7, 0x4F6, 0x4F5, 0x4F4,
   0x4F4, 0x5F4, 0x6F4, 0x7F4, 0x8F4, 0x9F4, 0xAF4, 0xBF4,
   0xCF4, 0xDF4, 0xEF4, 0xFF4, 0xFF4, 0xFE4, 0xFD4, 0xFC4,
   0xFB4, 0xFA4, 0xF94, 0xF84, 0xF74, 0xF64, 0xF54, 0xF44,
   0xF44, 0xF45, 0xF46, 0xF47, 0xF48, 0xF49, 0xF4A, 0xF4B};

void display_mandelbrot(fix32 start_x, fix32 start_y, fix32 diff){
  for (int i = 0; i < FB_HEIGHT >> RESOLUTION; ++i){
    for (int j = 0; j < FB_WIDTH >> RESOLUTION; ++j){
      struct Complex c = {start_x + diff * j, start_y - diff * i};
      int count = mandelbrot_count(&c);
      if (count >= 0){
        PIXEL_FB[i * FB_WIDTH + j] = colors[count % COLOR_COUNT];
      } else {
        PIXEL_FB[i * FB_WIDTH + j] = 0;
      }
    }
  }
}

int kernel_main(void){
  make_tiles_transparent();

  *PIXEL_SCALE = RESOLUTION;

  fix32 start_x = -0x00028000;
  fix32 start_y = 0x00012000;

  fix32 diff = 0x00000266 << RESOLUTION;

  display_mandelbrot(start_x, start_y, diff);

  while (getkey() != 'q');

  return 0;
}
