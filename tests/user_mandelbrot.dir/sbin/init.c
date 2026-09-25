/*
 * user_mandelbrot: compute-heavy user program for profiling and regression.
 *
 * Renders the same 80x60 Q16.16 Mandelbrot view as root/mandelbrot (same
 * origin, step, and 56-iteration cap, using its fix32.c) but prints checksums
 * instead of drawing, so it needs no VGA and exits on its own. Almost all of
 * its time is user-mode arithmetic, which makes it a useful workload for
 * measuring user vs. kernel time with the emulator's --profile window.
 *
 * Expected values in user_mandelbrot.ok were cross-checked against an
 * independent host model of the same 32-bit wrapping fixed-point math.
 */
#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"
#include "../../../root/mandelbrot/fix32.h"

// View parameters copied from root/mandelbrot/mandelbrot.c at RESOLUTION 0.
#define VIEW_WIDTH 80
#define VIEW_HEIGHT 60
#define MAX_ITERATIONS 56
#define START_X (-0x00028000)
#define START_Y 0x00012000
#define STEP (0x00000266 << 2)
// Multiplier for the order-sensitive row hash (a common small odd prime).
#define HASH_MULTIPLIER 31

// Count iterations before |z|^2 exceeds 4, or -1 if c stays bounded.
int mandelbrot_count(struct Complex* c){
  struct Complex z = {0, 0};
  int i;
  for (i = 0; i < MAX_ITERATIONS; ++i){
    struct Complex temp;
    mul_complex(&z, &z, &temp);
    add_complex(c, &temp, &z);
    fix32 d = norm(&z);
    if (d > 4 * FIXED_ONE) break;
  }
  if (i == MAX_ITERATIONS) i = -1;
  return i;
}

int main(void) {
  puts("***user mandelbrot start\n");

  unsigned total_iterations = 0;
  unsigned bounded_points = 0;
  unsigned hash = 0;
  for (int i = 0; i < VIEW_HEIGHT; ++i){
    for (int j = 0; j < VIEW_WIDTH; ++j){
      struct Complex c = {START_X + STEP * j, START_Y - STEP * i};
      int count = mandelbrot_count(&c);
      if (count < 0){
        bounded_points++;
      } else {
        total_iterations += count;
      }
      hash = hash * HASH_MULTIPLIER + (unsigned)(count + 1);
    }
  }

  unsigned results[3] = {total_iterations, bounded_points, hash};
  printf("***escape iterations %u, bounded points %u, hash 0x%X\n", results);
  puts("***user mandelbrot done\n");
  return 0;
}
