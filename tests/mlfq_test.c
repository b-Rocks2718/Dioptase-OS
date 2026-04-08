/*
 * Low-overhead visual MLFQ workload test.
 *
 * Validates:
 * - threads that do a tiny burst and then truly block stay near MLFQ level 0
 * - threads that burn enough CPU to demote once, then block, settle near
 *   MLFQ level 1
 * - fully CPU-bound threads sink to MLFQ level 2
 *
 * How:
 * - use the VGA text/tile framebuffer so each worker update only changes a
 *   few tiles
 * - keep every worker at NORMAL priority so the visible separation comes from
 *   MLFQ behavior, not from the priority-weighted scheduler
 * - top row workers do one tiny burst and then sleep, middle row workers do a
 *   short CPU burst until they reach level 1 and then sleep, and bottom row
 *   workers are CPU-bound
 * - the sleeping rows use small per-worker phase offsets so a one-core run
 *   does not collapse into one worker repeatedly getting the same post-wakeup
 *   slot every tick
 * - each worker panel contains tracks for levels 0/1/2; the '>' marker shows
 *   the current level, the moving '*' marker shows instantaneous CPU share, and
 *   the counter shows cumulative CPU share over time
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
#define START_ROW 10
#define START_COL 1
#define TRACK_LEN 10
#define UPDATE_DIVISOR 4
#define INPUT_SLEEP_JIFFIES 1
#define FINISH_WAIT_BUDGET 100000
#define INTERACTIVE_SLEEP_BASE 2
#define BURSTY_SLEEP_BASE 2
#define SLEEP_STAGGER_VARIANTS 2
#define COUNTER_DIGITS 5

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
  int sleep_jiffies;
  int initial_stagger_jiffies;
  enum WorkloadKind workload;
};

static int stop_visual = 0;
static int finished_workers = 0;

// Write one tile cell directly using the already-loaded text tileset.
static void put_tile_at(int x, int y, char c, int color) {
  TILE_FB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | c);
}

// Return one hex digit character.
static char hex_digit(unsigned value) {
  if (value < 10) return '0' + value;
  return 'A' + (value - 10);
}

// Draw a fixed-width hexadecimal counter.
static void draw_hex_counter(int x, int y, unsigned value, int digits, int color) {
  for (int digit = digits - 1; digit >= 0; digit--) {
    put_tile_at(x + digit, y, hex_digit(value & 0xF), color);
    value >>= 4;
  }
}

// Border/text color used for one workload class.
static int workload_color(enum WorkloadKind workload) {
  if (workload == WORKLOAD_INTERACTIVE) return 0xAF;
  if (workload == WORKLOAD_BURSTY) return 0x1F;
  return 0x1D;
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
  put_tile_at(x + 9, y + 1, 'C', border_color);
  put_tile_at(x + 10, y + 1, ':', border_color);
  draw_hex_counter(x + 11, y + 1, 0, COUNTER_DIGITS, border_color);

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

// Update the cumulative CPU-share counter for one worker.
static void set_cpu_counter(struct TileWorkloadArg* arg, unsigned cpu_steps) {
  draw_hex_counter(arg->panel_x + 11, arg->panel_y + 1,
                   cpu_steps, COUNTER_DIGITS, workload_color(arg->workload));
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

// Interactive workers do a tiny burst and then truly block, so they should
// stay near level 0. The per-worker sleep variation prevents same-phase wakeup
// convoys from dominating the one-core visual.
static void interactive_step(struct TileWorkloadArg* arg) {
  sleep(arg->sleep_jiffies);
}

// Bursty workers burn CPU until they reach level 1, then truly block so they
// settle there. The per-worker sleep variation serves the same anti-convoy
// purpose as the interactive row.
static void bursty_step(struct TileWorkloadArg* arg, int current_level) {
  if (current_level >= LEVEL_ONE) {
    sleep(arg->sleep_jiffies);
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
  unsigned cpu_steps;

  arg = (struct TileWorkloadArg*)raw_arg;
  previous_level = -1;
  previous_marker_pos = -1;
  marker_step = 0;
  last_drawn_step = -1;
  cpu_steps = 0;

  if (arg->initial_stagger_jiffies > 0) {
    sleep(arg->initial_stagger_jiffies);
  }

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

    cpu_steps++;
    marker_step++;
    if ((marker_step / UPDATE_DIVISOR) != last_drawn_step) {
      last_drawn_step = marker_step / UPDATE_DIVISOR;
      marker_pos = last_drawn_step % TRACK_LEN;
      set_marker(arg, current_level, previous_marker_pos, marker_pos);
      set_cpu_counter(arg, cpu_steps);
      previous_marker_pos = marker_pos;
    }

    if (arg->workload == WORKLOAD_INTERACTIVE) {
      interactive_step(arg);
    } else if (arg->workload == WORKLOAD_BURSTY) {
      bursty_step(arg, current_level);
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
  arg->sleep_jiffies = 0;
  arg->initial_stagger_jiffies = (workload * WORKERS_PER_ROW + worker_index) %
                                 (WORKERS_PER_ROW + 1);

  if (workload == WORKLOAD_INTERACTIVE) {
    arg->sleep_jiffies = INTERACTIVE_SLEEP_BASE +
                         (worker_index % SLEEP_STAGGER_VARIANTS);
  } else if (workload == WORKLOAD_BURSTY) {
    arg->sleep_jiffies = BURSTY_SLEEP_BASE +
                         (worker_index % SLEEP_STAGGER_VARIANTS);
  }

  draw_panel_frame(arg);

  fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "thread_mlfq_workloads_tiles: Fun allocation failed.\n");
  fun->func = workload_visual_worker;
  fun->arg = arg;

  thread_(fun, NORMAL_PRIORITY, ANY_CORE);
}

int kernel_main(void) {
  int row;
  int col;
  int panel_x;
  int panel_y;
  int total_workers;
  int wait_count;
  int key;
  enum WorkloadKind workload;

  load_text_tiles();
  clear_screen();

  say("MLFQ workload visual\n", NULL);
  say("All workers are NORMAL priority\n", NULL);
  say("Top row: tiny burst then staggered sleep, so it stays near level 0\n", NULL);
  say("Mid row: short burst then staggered sleep, so it settles near level 1\n", NULL);
  say("Bot row: CPU-bound, so it sinks to level 2\n", NULL);
  say("> shows current MLFQ level, * shows CPU share\n", NULL);
  say("C:xxxxx is cumulative run count in hex\n", NULL);
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

  while (true) {
    key = waitkey();
    if (key == 'q' || key == 'Q') break;
    sleep(INPUT_SLEEP_JIFFIES);
  }

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
