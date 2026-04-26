#include "fix32.h"

// Multiply two Q16.16 fixed-point values without requiring 64-bit arithmetic.
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

// Add two complex values component-wise.
void add_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out){
  out->x = a->x + b->x;
  out->y = a->y + b->y;
}

// Multiply two complex values in Q16.16 form.
void mul_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out){
  out->x = mul_fixed(a->x, b->x) - mul_fixed(a->y, b->y);
  out->y = mul_fixed(a->x, b->y) + mul_fixed(a->y, b->x);
}

// Compute the squared magnitude of one complex value.
fix32 norm(struct Complex* z){
  return mul_fixed(z->x, z->x) + mul_fixed(z->y, z->y);
}
