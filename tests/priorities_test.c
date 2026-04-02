/*
 * Low-overhead visual MLFQ scheduler test.
 *
 * Validates:
 * - CPU-bound workers demote from MLFQ level 0 to level 2 as they burn quanta
 * - periodic boosts move workers back to level 0
 * - higher-priority workers receive more CPU time than lower-priority workers
 *
 * How:
 * - use the VGA text/tile framebuffer instead of the pixel framebuffer so each
 *   worker update touches only a handful of tiles
 * - build a 3x4 grid of worker panels: top row HIGH priority, middle NORMAL,
 *   bottom LOW
 * - inside each panel, the three track rows correspond to MLFQ levels 0, 1,
 *   and 2
 * - a moving marker shows instantaneous CPU share, a cumulative counter shows
 *   total CPU share over time, and an arrow jumps between the 0/1/2 rows when
 *   the worker's current MLFQ level changes
 */

#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/machine.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/ps2.h"
#include "../kernel/vga.h"
#include "../kernel/scheduler.h"
#include "../kernel/interrupts.h"
#include "../kernel/config.h"
#include "../kernel/per_core.h"

#define PRIORITY_ROWS 3
#define WORKERS_PER_ROW 4
#define PANEL_WIDTH 18
#define PANEL_HEIGHT 6
#define PANEL_GAP_X 2
#define PANEL_GAP_Y 1
#define START_ROW 10
#define START_COL 1
#define TRACK_LEN 10
#define UPDATE_DIVISOR 4
#define INPUT_SLEEP_JIFFIES 100
#define FINISH_WAIT_BUDGET 100000
#define COUNTER_DIGITS 5

struct TileVisualArg {
  int panel_x;
  int panel_y;
  int worker_index;
  int marker_color;
  enum ThreadPriority priority;
};

static int stop_visual = 0;
static int finished_workers = 0;

// Write one tile cell directly. The text tileset is already loaded.
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

// Fill a rectangle in tile space with one character/color pair.
static void fill_tile_rect(int x, int y, int width, int height, char c, int color) {
  int row;
  int col;

  for (row = y; row < y + height; row++) {
    for (col = x; col < x + width; col++) {
      put_tile_at(col, row, c, color);
    }
  }
}

// Border/text color used for one scheduler priority.
static int priority_color(enum ThreadPriority priority) {
  if (priority == HIGH_PRIORITY) return 0x1D;
  if (priority == NORMAL_PRIORITY) return 0xFC;
  return 0xE8;
}

// Draw the static frame and labels for one worker panel.
static void draw_panel_frame(struct TileVisualArg* arg) {
  int x;
  int y;
  int border_color;
  int track_row;
  int col;

  x = arg->panel_x;
  y = arg->panel_y;
  border_color = priority_color(arg->priority);

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

  put_tile_at(x + 2, y + 1,
              arg->priority == HIGH_PRIORITY ? 'H' :
              (arg->priority == NORMAL_PRIORITY ? 'N' : 'L'),
              border_color);
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
static void set_level_indicator(struct TileVisualArg* arg, int previous_level, int current_level) {
  int x;
  int y;
  int border_color;

  x = arg->panel_x;
  y = arg->panel_y;
  border_color = priority_color(arg->priority);

  if (previous_level >= LEVEL_ZERO && previous_level <= LEVEL_TWO) {
    put_tile_at(x + 4, y + 2 + previous_level, ' ', border_color);
  }

  put_tile_at(x + 4, y + 2 + current_level, '>', border_color);
  put_tile_at(x + 7, y + 1, '0' + current_level, border_color);
}

// Move the marker on one worker's current MLFQ track.
static void set_marker(struct TileVisualArg* arg, int level, int previous_pos, int current_pos) {
  int row_y;

  row_y = arg->panel_y + 2 + level;

  if (previous_pos >= 0) {
    put_tile_at(arg->panel_x + 5 + previous_pos, row_y, '.', 0x92);
  }

  put_tile_at(arg->panel_x + 5 + current_pos, row_y, '*', arg->marker_color);
}

// Update the cumulative CPU-share counter for one worker.
static void set_cpu_counter(struct TileVisualArg* arg, unsigned cpu_steps) {
  draw_hex_counter(arg->panel_x + 11, arg->panel_y + 1,
                   cpu_steps, COUNTER_DIGITS, priority_color(arg->priority));
}

// CPU-bound worker that renders its own scheduler state with only a few tile writes.
static void tile_visual_worker(void* raw_arg) {
  struct TileVisualArg* arg;
  int current_level;
  int previous_level;
  int marker_step;
  int marker_pos;
  int previous_marker_pos;
  int last_drawn_step;
  unsigned cpu_steps;
  unsigned was;
  struct TCB* self;

  arg = (struct TileVisualArg*)raw_arg;
  previous_level = -1;
  previous_marker_pos = -1;
  marker_step = 0;
  last_drawn_step = -1;
  cpu_steps = 0;

  while (__atomic_load_n(&stop_visual) == 0) {
    // Read the current thread's MLFQ level with interrupts disabled so
    // get_current_tcb() satisfies its documented preconditions.
    was = interrupts_disable();
    self = get_current_tcb();
    current_level = self->mlfq_level;
    interrupts_restore(was);

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
  }

  __atomic_fetch_add(&finished_workers, 1);
}

// Create one worker panel and launch the corresponding visual worker.
static void spawn_tile_visual_worker(int panel_x, int panel_y,
                                     enum ThreadPriority priority, int worker_index) {
  struct TileVisualArg* arg;
  struct Fun* fun;
  
  arg = malloc(sizeof(struct TileVisualArg));
  assert(arg != NULL,
         "thread_mlfq_visual_tiles: TileVisualArg allocation failed.\n");
  arg->panel_x = panel_x;
  arg->panel_y = panel_y;
  arg->worker_index = worker_index;
  arg->marker_color = priority_color(priority);
  arg->priority = priority;

  draw_panel_frame(arg);

  fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "thread_mlfq_visual_tiles: Fun allocation failed.\n");
  fun->func = tile_visual_worker;
  fun->arg = arg;

  thread_priority(fun, priority);
}

int kernel_main(void) {
  int row;
  int col;
  int panel_x;
  int panel_y;
  int total_workers;
  int wait_count;
  int key;
  enum ThreadPriority priority;

  load_text_tiles();
  clear_screen();

  say("MLFQ tile visual\n", NULL);
  say("Rows: HIGH / NORMAL / LOW priority\n", NULL);
  say("Inside each box, tracks 0/1/2 are MLFQ levels\n", NULL);
  say("> jumps when the worker changes MLFQ level\n", NULL);
  say("* moves faster when that worker gets more CPU time\n", NULL);
  say("C:xxxxx is cumulative run count in hex\n", NULL);
  say("Press q to exit\n", NULL);

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

    panel_y = START_ROW + (row * (PANEL_HEIGHT + PANEL_GAP_Y));
    for (col = 0; col < WORKERS_PER_ROW; col++) {
      panel_x = START_COL + (col * (PANEL_WIDTH + PANEL_GAP_X));
      spawn_tile_visual_worker(panel_x, panel_y, priority, col);
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
