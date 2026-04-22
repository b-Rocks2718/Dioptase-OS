#ifndef FIX32_H
#define FIX32_H

#define fix32 int

#define FIXED_ONE 0x00010000

struct Complex {
  fix32 x;
  fix32 y;
};

// Multiply two Q16.16 fixed-point values without requiring 64-bit arithmetic.
fix32 mul_fixed(fix32 a, fix32 b);

// Add two complex values component-wise.
void add_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out);
          
// Multiply two complex values in Q16.16 form.
void mul_complex(struct Complex* a, struct Complex* b, 
                 struct Complex* out);

// Compute the squared magnitude of one complex value.
fix32 norm(struct Complex* z);

#endif // FIX32_H
