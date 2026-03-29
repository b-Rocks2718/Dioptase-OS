/*
 * Thread priorities test
 *
 * Validates:
 * - threads are scheduled according to their priorities
 * - Higher priority threads get more CPU time than lower priority threads
 * - All threads can still make progress
 *
 * How:
 * - split the framebuffer into 16 squares
 * - launch 16 worker threads, each worker fills in a square with a solid color
 * - artificially slow down workers with a loop, so the rate they do work if visible
 * - higher priority threads should finish before lower priority ones
 */

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
#include "../kernel/scheduler.h"

#define RESOLUTION 2
#define LOOP_ITERS 1000

// Render one framebuffer quadrant described by the five-word argument array.
void fill_square(void* arg){
  int* arg_arr = (int*)arg;  
  int start_j = arg_arr[0];
  int start_i = arg_arr[1];

  for (int i = start_i; i < start_i + ((FB_HEIGHT/4) >> RESOLUTION); ++i){
    for (int j = start_j; j < start_j + ((FB_WIDTH/4) >> RESOLUTION); ++j){
      // spin for a while to waste time

      for (int k = 0; k < LOOP_ITERS; k++);

      PIXEL_FB[i * FB_WIDTH + j] = 0xFE4;;
    }
  }
}

// Split the framebuffer into quadrants and render them across four threads.
int kernel_main(void){
  make_tiles_transparent();

  *PIXEL_SCALE = RESOLUTION;

  struct Fun* fun1 = (struct Fun*)malloc(sizeof(struct Fun));
  fun1->func = (void*)fill_square;
  fun1->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun1->arg)[0] = 0;
  ((int*)fun1->arg)[1] = 0;

  struct Fun* fun2 = (struct Fun*)malloc(sizeof(struct Fun));
  fun2->func = (void*)fill_square;
  fun2->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun2->arg)[0] = (FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun2->arg)[1] = 0;

  struct Fun* fun3 = (struct Fun*)malloc(sizeof(struct Fun));
  fun3->func = (void*)fill_square;
  fun3->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun3->arg)[0] = (FB_WIDTH/2) >> RESOLUTION;
  ((int*)fun3->arg)[1] = 0;

  struct Fun* fun4 = (struct Fun*)malloc(sizeof(struct Fun));
  fun4->func = (void*)fill_square;
  fun4->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun4->arg)[0] = (3 * FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun4->arg)[1] = 0;

  struct Fun* fun5 = (struct Fun*)malloc(sizeof(struct Fun));
  fun5->func = (void*)fill_square;
  fun5->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun5->arg)[0] = 0;
  ((int*)fun5->arg)[1] = (FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun6 = (struct Fun*)malloc(sizeof(struct Fun));
  fun6->func = (void*)fill_square;
  fun6->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun6->arg)[0] = (FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun6->arg)[1] = (FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun7 = (struct Fun*)malloc(sizeof(struct Fun));
  fun7->func = (void*)fill_square;
  fun7->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun7->arg)[0] = (FB_WIDTH/2) >> RESOLUTION;
  ((int*)fun7->arg)[1] = (FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun8 = (struct Fun*)malloc(sizeof(struct Fun));
  fun8->func = (void*)fill_square;
  fun8->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun8->arg)[0] = (3 * FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun8->arg)[1] = (FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun9 = (struct Fun*)malloc(sizeof(struct Fun));
  fun9->func = (void*)fill_square;
  fun9->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun9->arg)[0] = 0;
  ((int*)fun9->arg)[1] = (FB_HEIGHT/2) >> RESOLUTION;

  struct Fun* fun10 = (struct Fun*)malloc(sizeof(struct Fun));
  fun10->func = (void*)fill_square;
  fun10->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun10->arg)[0] = (FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun10->arg)[1] = (FB_HEIGHT/2) >> RESOLUTION;

  struct Fun* fun11 = (struct Fun*)malloc(sizeof(struct Fun));
  fun11->func = (void*)fill_square;
  fun11->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun11->arg)[0] = (FB_WIDTH/2) >> RESOLUTION;
  ((int*)fun11->arg)[1] = (FB_HEIGHT/2) >> RESOLUTION;

  struct Fun* fun12 = (struct Fun*)malloc(sizeof(struct Fun));
  fun12->func = (void*)fill_square;
  fun12->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun12->arg)[0] = (3 * FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun12->arg)[1] = (FB_HEIGHT/2) >> RESOLUTION;

  struct Fun* fun13 = (struct Fun*)malloc(sizeof(struct Fun));
  fun13->func = (void*)fill_square;
  fun13->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun13->arg)[0] = 0;
  ((int*)fun13->arg)[1] = (3 * FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun14 = (struct Fun*)malloc(sizeof(struct Fun));
  fun14->func = (void*)fill_square;
  fun14->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun14->arg)[0] = (FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun14->arg)[1] = (3 * FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun15 = (struct Fun*)malloc(sizeof(struct Fun));
  fun15->func = (void*)fill_square;
  fun15->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun15->arg)[0] = (FB_WIDTH/2) >> RESOLUTION;
  ((int*)fun15->arg)[1] = (3 * FB_HEIGHT/4) >> RESOLUTION;

  struct Fun* fun16 = (struct Fun*)malloc(sizeof(struct Fun));
  fun16->func = (void*)fill_square;
  fun16->arg = (void*)malloc(2 * sizeof(int));
  ((int*)fun16->arg)[0] = (3 * FB_WIDTH/4) >> RESOLUTION;
  ((int*)fun16->arg)[1] = (3 * FB_HEIGHT/4) >> RESOLUTION;

  set_priority(LOW_PRIORITY);

  thread_priority(fun1, HIGH_PRIORITY);
  thread_priority(fun2, HIGH_PRIORITY);
  thread_priority(fun3, HIGH_PRIORITY);
  thread_priority(fun4, HIGH_PRIORITY);
  thread_priority(fun5, NORMAL_PRIORITY);
  thread_priority(fun6, NORMAL_PRIORITY);
  thread_priority(fun7, NORMAL_PRIORITY);
  thread_priority(fun8, NORMAL_PRIORITY);
  thread_priority(fun9, LOW_PRIORITY);
  thread_priority(fun10, LOW_PRIORITY);
  thread_priority(fun11, LOW_PRIORITY);
  thread_priority(fun12, LOW_PRIORITY);
  thread_priority(fun13, NORMAL_PRIORITY);
  thread_priority(fun14, NORMAL_PRIORITY);
  thread_priority(fun15, NORMAL_PRIORITY);
  thread_priority(fun16, NORMAL_PRIORITY);

  while (waitkey() != 'q');

  return 0;
}
