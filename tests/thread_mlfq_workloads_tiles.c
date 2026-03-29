/*
 * Low-overhead visual MLFQ workload test.
 *
 * Validates:
 * - threads that voluntarily yield immediately stay near MLFQ level 0
 * - threads that burn enough CPU to demote once, then yield, settle near
 *   MLFQ level 1
 * - fully CPU-bound threads sink to MLFQ level 2
 *
 * How:
 * - use the VGA text/tile framebuffer so each worker update only changes a
 *   few tiles
 * - keep every worker at NORMAL priority so the visible separation comes from
 *   MLFQ behavior, not from the priority-weighted scheduler
 * - top row workers are interactive/yield-heavy, middle row workers do a short
 *   CPU burst until they reach level 1 and then yield, and bottom row workers
 *   are CPU-bound
 * - each worker panel contains tracks for levels 0/1/2; the '>' marker shows
 *   the current level and the moving '*' marker shows how much CPU that worker
 *   is receiving
 */

#include "../kernel/constants.h"
#include "../kernel/machine.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/ps2.h"
#include "../kernel/vga.h"
#include "../kernel/scheduler.h"
#include "../kernel/interrupts.h"
#include "../kernel/per_core.h"

#define WORKLOAD_ROWS 3
#define WORKERS_PER_ROW 4
#define PANEL_WIDTH 18
#define PANEL_HEIGHT 6
#define PANEL_GAP_X 2
#define PANEL_GAP_Y 1
#define START_ROW 6
#define START_COL 1
#define TRACK_LEN 10
#define UPDATE_DIVISOR 8
#define FINISH_WAIT_BUDGET 100000

enum WorkloadKind {
  WORKLOAD_INTERACTIVE = 0,
  WORKLOAD_BURSTY = 1,
  WORKLOAD_CPU_BOUND = 2,
};

struct TileWorkloadArg {
  int panel_x;
  int panel_y;
  int worker_index;
  int marker_color;
  enum WorkloadKind workload;
};

static int stop_visual = 0;
static int finished_workers = 0;

// Write one tile cell directly using the already-loaded text tileset.
static void put_tile_at(int x, int y, char c, int color) {
  TILE_FB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | c);
}

// Border/text color used for one workload class.
static int workload_color(enum WorkloadKind workload) {
  if (workload == WORKLOAD_INTERACTIVE) return 0xAF;
  if (workload == WORKLOAD_BURSTY) return 0xFC;
  return 0xF0;
}

// One-letter label used in the panel header.
static char workload_letter(enum WorkloadKind workload) {
  if (workload == WORKLOAD_INTERACTIVE) return 'I';
  if (workload == WORKLOAD_BURSTY) return 'B';
  return 'C';
}

// Draw the static frame and labels for one worker panel.
static void draw_panel_frame(struct TileWorkloadArg* arg) {
  int x;
  int y;
  int border_color;
  int track_row;
  int col;

  x = arg->panel_x;
  y = arg->panel_y;
  border_color = workload_color(arg->workload);

  put_tile_at(x, y, '+', border_color);
  put_tile_at(x + PANEL_WIDTH - 1, y, '+', border_color);
  put_tile_at(x, y + PANEL_HEIGHT - 1, '+', border_color);
  put_tile_at(x + PANEL_WIDTH - 1, y + PANEL_HEIGHT - 1, '+', border_color);

  for (col = x + 1; col < x + PANEL_WIDTH - 1; col++) {
    put_tile_at(col, y, '-', border_color);
    put_tile_at(col, y + PANEL_HEIGHT - 1, '-', border_color);
  }

  for (track_row = y + 1; track_row < y + PANEL_HEIGHT - 1; track_row++) {
    put_tile_at(x, track_row, '|', border_color);
    put_tile_at(x + PANEL_WIDTH - 1, track_row, '|', border_color);
  }

  put_tile_at(x + 2, y + 1, workload_letter(arg->workload), border_color);
  put_tile_at(x + 3, y + 1, '0' + arg->worker_index, border_color);
  put_tile_at(x + 5, y + 1, 'L', border_color);
  put_tile_at(x + 6, y + 1, ':', border_color);
  put_tile_at(x + 7, y + 1, '?', border_color);

  for (track_row = 0; track_row < MLFQ_LEVELS; track_row++) {
    int row_y = y + 2 + track_row;
    put_tile_at(x + 2, row_y, '0' + track_row, border_color);
    put_tile_at(x + 3, row_y, ':', border_color);
    put_tile_at(x + 4, row_y, ' ', border_color);
    for (col = 0; col < TRACK_LEN; col++) {
      put_tile_at(x + 5 + col, row_y, '.', 0x92);
    }
  }
}

// Update the currently selected MLFQ level indicator inside one panel.
static void set_level_indicator(struct TileWorkloadArg* arg,
                                int previous_level, int current_level) {
  int x;
  int y;
  int border_color;

  x = arg->panel_x;
  y = arg->panel_y;
  border_color = workload_color(arg->workload);

  if (previous_level >= LEVEL_ZERO && previous_level <= LEVEL_TWO) {
    put_tile_at(x + 4, y + 2 + previous_level, ' ', border_color);
  }

  put_tile_at(x + 4, y + 2 + current_level, '>', border_color);
  put_tile_at(x + 7, y + 1, '0' + current_level, border_color);
}

// Move the marker on one worker's current MLFQ track.
static void set_marker(struct TileWorkloadArg* arg,
                       int level, int previous_pos, int current_pos) {
  int row_y;

  row_y = arg->panel_y + 2 + level;

  if (previous_pos >= 0) {
    put_tile_at(arg->panel_x + 5 + previous_pos, row_y, '.', 0x92);
  }

  put_tile_at(arg->panel_x + 5 + current_pos, row_y, '*', arg->marker_color);
}

// Read the current thread's MLFQ level with the per-core accessor preconditions satisfied.
static int read_current_level(void) {
  unsigned was;
  struct TCB* self;
  int level;

  was = interrupts_disable();
  self = get_current_tcb();
  level = self->mlfq_level;
  interrupts_restore(was);

  return level;
}

// Interactive workers yield immediately, so they should stay near level 0.
static void interactive_step(void) {
  yield();
}

// Bursty workers burn CPU until they reach level 1, then become cooperative.
static void bursty_step(int current_level) {
  if (current_level >= LEVEL_ONE) {
    yield();
  }
}

// CPU-bound workers never block or yield.
static void cpu_bound_step(void) {
}

// Worker that renders its own MLFQ state while following one workload pattern.
static void workload_visual_worker(void* raw_arg) {
  struct TileWorkloadArg* arg;
  int current_level;
  int previous_level;
  int marker_step;
  int marker_pos;
  int previous_marker_pos;
  int last_drawn_step;

  arg = (struct TileWorkloadArg*)raw_arg;
  previous_level = -1;
  previous_marker_pos = -1;
  marker_step = 0;
  last_drawn_step = -1;

  while (__atomic_load_n(&stop_visual) == 0) {
    current_level = read_current_level();

    if (current_level != previous_level) {
      if (previous_level >= LEVEL_ZERO && previous_level <= LEVEL_TWO &&
          previous_marker_pos >= 0) {
        put_tile_at(arg->panel_x + 5 + previous_marker_pos,
                    arg->panel_y + 2 + previous_level,
                    '.', 0x92);
      }
      set_level_indicator(arg, previous_level, current_level);
      previous_level = current_level;
      previous_marker_pos = -1;
    }

    marker_step++;
    if ((marker_step / UPDATE_DIVISOR) != last_drawn_step) {
      last_drawn_step = marker_step / UPDATE_DIVISOR;
      marker_pos = last_drawn_step % TRACK_LEN;
      set_marker(arg, current_level, previous_marker_pos, marker_pos);
      previous_marker_pos = marker_pos;
    }

    if (arg->workload == WORKLOAD_INTERACTIVE) {
      interactive_step();
    } else if (arg->workload == WORKLOAD_BURSTY) {
      bursty_step(current_level);
    } else {
      cpu_bound_step();
    }
  }

  __atomic_fetch_add(&finished_workers, 1);
}

// Create one worker panel and launch the corresponding visual worker.
static void spawn_workload_visual_worker(int panel_x, int panel_y,
                                         enum WorkloadKind workload, int worker_index) {
  struct TileWorkloadArg* arg;
  struct Fun* fun;

  arg = malloc(sizeof(struct TileWorkloadArg));
  assert(arg != NULL,
         "thread_mlfq_workloads_tiles: TileWorkloadArg allocation failed.\n");
  arg->panel_x = panel_x;
  arg->panel_y = panel_y;
  arg->worker_index = worker_index;
  arg->marker_color = workload_color(workload);
  arg->workload = workload;

  draw_panel_frame(arg);

  fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "thread_mlfq_workloads_tiles: Fun allocation failed.\n");
  fun->func = workload_visual_worker;
  fun->arg = arg;

  thread_priority(fun, NORMAL_PRIORITY);
}

int kernel_main(void) {
  int row;
  int col;
  int panel_x;
  int panel_y;
  int total_workers;
  int wait_count;
  enum WorkloadKind workload;

  load_text_tiles();
  clear_screen();

  say("MLFQ workload visual\n", NULL);
  say("All workers are NORMAL priority\n", NULL);
  say("Top row: interactive/yield-heavy\n", NULL);
  say("Mid row: short burst then yield, so it settles near level 1\n", NULL);
  say("Bot row: CPU-bound, so it sinks to level 2\n", NULL);
  say("> shows current MLFQ level, * shows CPU share\n", NULL);
  say("Press q to exit\n", NULL);

  total_workers = WORKLOAD_ROWS * WORKERS_PER_ROW;
  set_priority(LOW_PRIORITY);

  for (row = 0; row < WORKLOAD_ROWS; row++) {
    workload = (enum WorkloadKind)row;
    panel_y = START_ROW + (row * (PANEL_HEIGHT + PANEL_GAP_Y));
    for (col = 0; col < WORKERS_PER_ROW; col++) {
      panel_x = START_COL + (col * (PANEL_WIDTH + PANEL_GAP_X));
      spawn_workload_visual_worker(panel_x, panel_y, workload, col);
    }
  }

  while (waitkey() != 'q');

  __atomic_store_n(&stop_visual, 1);
  set_priority(HIGH_PRIORITY);

  for (wait_count = 0;
       wait_count < FINISH_WAIT_BUDGET &&
       __atomic_load_n(&finished_workers) != total_workers;
       wait_count++) {
    yield();
  }

  return 0;
}
