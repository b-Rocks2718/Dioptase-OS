/*
 * Visual MLFQ scheduler test.
 *
 * Validates:
 * - CPU-bound threads visibly demote from level 0 to level 2 as they burn
 *   their quanta
 * - periodic MLFQ boosts visibly return those threads to level 0
 * - higher-priority threads make more visible progress than lower-priority
 *   threads while still sharing the CPU within each priority class
 *
 * How:
 * - split the pixel framebuffer into three priority rows and four worker cells
 *   per row
 * - each cell is divided into three horizontal bands, one for each MLFQ level
 * - a worker repeatedly samples its own scheduler state, highlights its active
 *   MLFQ band, and advances a marker inside that band whenever it gets CPU time
 * - top row workers are HIGH priority, middle row NORMAL, and bottom row LOW,
 *   so row-to-row marker speed shows priority weighting while band changes show
 *   MLFQ demotion and boost behavior
 */

#include "../kernel/machine.h"
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/threads.h"
#include "../kernel/config.h"
#include "../kernel/ps2.h"
#include "../kernel/debug.h"
#include "../kernel/vga.h"
#include "../kernel/scheduler.h"
#include "../kernel/interrupts.h"
#include "../kernel/per_core.h"

#define RESOLUTION 0
#define PRIORITY_ROWS 3
#define WORKERS_PER_ROW 4
#define CELL_GAP 6
#define CELL_BORDER 2
#define BAND_GAP 3
#define MARKER_WIDTH 12
#define FINISH_WAIT_BUDGET 100000

struct VisualArg {
  int x;
  int y;
  int width;
  int height;
  int band_height;
  int marker_height;
  enum ThreadPriority priority;
};

static int stop_visual = 0;
static int finished_workers = 0;

// Fill one rectangle in the pixel framebuffer.
static void fill_rect(int x, int y, int width, int height, short color) {
  int i;
  int j;

  for (i = y; i < y + height; i++) {
    for (j = x; j < x + width; j++) {
      PIXEL_FB[i * FB_WIDTH + j] = color;
    }
  }
}

// Return the border color that identifies one thread priority row.
static short priority_border_color(enum ThreadPriority priority) {
  if (priority == HIGH_PRIORITY) return 0x0FF;
  if (priority == NORMAL_PRIORITY) return 0xFFF;
  return 0x66F;
}

// Return the background color for one MLFQ level.
static short level_color(int level, bool active) {
  if (level == LEVEL_ZERO) return active ? 0x2E2 : 0x041;
  if (level == LEVEL_ONE) return active ? 0xFD0 : 0x630;
  return active ? 0xF33 : 0x300;
}

// Return the moving marker color for one thread priority.
static short marker_color(enum ThreadPriority priority) {
  if (priority == HIGH_PRIORITY) return 0x0FF;
  if (priority == NORMAL_PRIORITY) return 0xFFF;
  return 0x88F;
}

// Draw one worker cell. Exactly one band is active at a time.
static void draw_worker_cell(struct VisualArg* arg, int active_level) {
  int inner_x;
  int inner_y;
  int inner_width;
  int level;
  int band_y;
  short border;

  border = priority_border_color(arg->priority);
  fill_rect(arg->x, arg->y, arg->width, arg->height, border);

  inner_x = arg->x + CELL_BORDER;
  inner_y = arg->y + CELL_BORDER;
  inner_width = arg->width - (2 * CELL_BORDER);

  fill_rect(inner_x, inner_y, inner_width, arg->height - (2 * CELL_BORDER), 0x000);

  for (level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    band_y = inner_y + (level * (arg->band_height + BAND_GAP));
    fill_rect(inner_x, band_y, inner_width, arg->band_height,
              level_color(level, level == active_level));
  }
}

// Draw the moving marker for one worker.
static void draw_marker(struct VisualArg* arg, int level, int marker_x, short color) {
  int marker_y;

  marker_y = arg->y + CELL_BORDER +
    (level * (arg->band_height + BAND_GAP)) +
    ((arg->band_height - arg->marker_height) / 2);

  fill_rect(marker_x, marker_y, MARKER_WIDTH, arg->marker_height, color);
}

// CPU-bound worker that turns its own cell into a live MLFQ state display.
static void visual_worker(void* raw_arg) {
  struct VisualArg* arg;
  int current_level;
  int previous_level;
  int previous_marker_x;
  int travel;
  int phase;
  int period;
  int offset;
  int marker_x;
  short bg_color;
  short accent;
  unsigned was;
  struct TCB* self;

  arg = (struct VisualArg*)raw_arg;
  previous_level = -1;
  previous_marker_x = -1;
  phase = 0;
  accent = marker_color(arg->priority);
  travel = arg->width - (2 * CELL_BORDER) - MARKER_WIDTH;
  if (travel < 0) travel = 0;
  period = (2 * travel) + 1;
  if (period <= 0) period = 1;

  while (__atomic_load_n(&stop_visual) == 0) {
    // Read our own TCB with interrupts disabled so the per-core accessor's
    // preconditions hold without changing the scheduler's can_preempt policy.
    was = interrupts_disable();
    self = get_current_tcb();
    current_level = self->mlfq_level;
    interrupts_restore(was);

    if (current_level != previous_level) {
      draw_worker_cell(arg, current_level);
      previous_level = current_level;
      previous_marker_x = -1;
    }

    bg_color = level_color(current_level, true);

    if (previous_marker_x >= 0) {
      draw_marker(arg, current_level, previous_marker_x, bg_color);
    }

    offset = phase;
    if (offset > travel) {
      offset = (2 * travel) - offset;
    }
    marker_x = arg->x + CELL_BORDER + offset;
    draw_marker(arg, current_level, marker_x, accent);

    previous_marker_x = marker_x;
    phase++;
    if (phase >= period) {
      phase = 0;
    }
  }

  __atomic_fetch_add(&finished_workers, 1);
}

// Allocate and launch one visual worker for the requested cell.
static void spawn_visual_worker(int x, int y, int width, int height,
                                enum ThreadPriority priority) {
  struct VisualArg* arg;
  struct Fun* fun;

  arg = malloc(sizeof(struct VisualArg));
  assert(arg != NULL, "thread_mlfq_visual: VisualArg allocation failed.\n");
  arg->x = x;
  arg->y = y;
  arg->width = width;
  arg->height = height;
  arg->band_height = (height - (2 * CELL_BORDER) - (2 * BAND_GAP)) / MLFQ_LEVELS;
  arg->marker_height = arg->band_height - 8;
  if (arg->marker_height < 4) {
    arg->marker_height = 4;
  }
  arg->priority = priority;

  draw_worker_cell(arg, -1);

  fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "thread_mlfq_visual: Fun allocation failed.\n");
  fun->func = visual_worker;
  fun->arg = arg;

  thread_priority(fun, priority);
}

int kernel_main(void) {
  int cell_width;
  int cell_height;
  int row;
  int col;
  int x;
  int y;
  int total_workers;
  enum ThreadPriority priority;

  say("| thread_mlfq_visual: top row=HIGH, middle=NORMAL, bottom=LOW\n", NULL);
  say("| thread_mlfq_visual: top/middle/bottom band = MLFQ level 0/1/2\n", NULL);
  say("| thread_mlfq_visual: band color shows current level, marker speed shows CPU share\n", NULL);
  say("| thread_mlfq_visual: periodic jumps back to the top band are MLFQ boosts\n", NULL);
  say("| thread_mlfq_visual: press q to exit\n", NULL);

  make_tiles_transparent();
  *PIXEL_SCALE = RESOLUTION;

  fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x000);

  cell_width = (FB_WIDTH - ((WORKERS_PER_ROW + 1) * CELL_GAP)) / WORKERS_PER_ROW;
  cell_height = (FB_HEIGHT - ((PRIORITY_ROWS + 1) * CELL_GAP)) / PRIORITY_ROWS;

  total_workers = PRIORITY_ROWS * WORKERS_PER_ROW;
  set_priority(LOW_PRIORITY);

  for (row = 0; row < PRIORITY_ROWS; row++) {
    if (row == 0) {
      priority = HIGH_PRIORITY;
    } else if (row == 1) {
      priority = NORMAL_PRIORITY;
    } else {
      priority = LOW_PRIORITY;
    }

    y = CELL_GAP + (row * (cell_height + CELL_GAP));
    for (col = 0; col < WORKERS_PER_ROW; col++) {
      x = CELL_GAP + (col * (cell_width + CELL_GAP));
      spawn_visual_worker(x, y, cell_width, cell_height, priority);
    }
  }

  while (waitkey() != 'q');

  __atomic_store_n(&stop_visual, 1);
  set_priority(HIGH_PRIORITY);

  for (row = 0; row < FINISH_WAIT_BUDGET &&
                  __atomic_load_n(&finished_workers) != total_workers;
       row++) {
    yield();
  }

  return 0;
}
