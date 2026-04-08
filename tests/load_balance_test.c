/*
 * Large approximate queue visual for scheduler load balancing.
 *
 * Validates:
 * - sleeping or temporarily blocked workers are shown in a dedicated sleep box
 *   instead of appearing runnable
 * - ANY_CORE workers that wake from sleep first become runnable in the shared
 *   global ready queue
 * - idle cores admit that shared-global work into their own local ready queues
 * - some workers pin themselves to a chosen core, after which they stop
 *   migrating and only reappear in that core's local queue
 * - MLFQ level remains visible because each queue box is split into rows 0/1/2
 *
 * How:
 * - draw one full-width global ready-queue box and four wide per-core local
 *   queue boxes so the queue graphics use most of the tile screen
 * - draw one full-width sleep/block box below the queue graphics and legend so
 *   sleeping workers do not look runnable
 * - inside each box, rows 0/1/2 correspond to MLFQ levels 0/1/2
 * - color each worker token by static priority using the same palette as the
 *   existing priority visual: HIGH=0x1D, NORMAL=0xFC, LOW=0xE8
 * - kernel_main draws test-owned global/local/sleep state, and ANY_CORE
 *   workers briefly publish GLOBAL when they wake so their shared-ready path
 *   remains visible without reading scheduler queue internals
 *
 * This test is intentionally mixed-accuracy. The boxes show the queue or
 * blocked state a worker most recently entered. The scheduling docs specify
 * wakeup and admission behavior, but not any architectural debug-visualization
 * interface, so this visual avoids reading live scheduler queue internals.
 */

#include "../kernel/atomic.h"
#include "../kernel/config.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/interrupts.h"
#include "../kernel/machine.h"
#include "../kernel/per_core.h"
#include "../kernel/print.h"
#include "../kernel/ps2.h"
#include "../kernel/scheduler.h"
#include "../kernel/threads.h"
#include "../kernel/vga.h"

#define NUM_ANY_WORKERS 24
#define NUM_PINNED_PER_CORE 3
#define NUM_PINNED_WORKERS 12
#define TOTAL_WORKERS 36
#define NUM_ANY_WORKERS_PER_PRIORITY 8
#define ANY_CORE_LEGEND_FIRST_ROW 12

#define SLEEP_BOX_X 0
#define SLEEP_BOX_Y 38
#define SLEEP_BOX_WIDTH 80
#define SLEEP_VISIBLE_SLOTS 38

#define GLOBAL_BOX_X 0
#define GLOBAL_BOX_Y 8
#define GLOBAL_BOX_WIDTH 80
#define GLOBAL_VISIBLE_SLOTS 38

#define LOCAL_BOX_Y 17
#define LOCAL_BOX_WIDTH 20
#define LOCAL_VISIBLE_SLOTS 8

#define BOX_HEIGHT 6
#define SLOT_WIDTH 2
#define MAX_SNAPSHOT_SLOTS 38

#define LEGEND_ROW_ONE 25
#define LEGEND_ROW_TWO 26
#define LEGEND_ROW_THREE 28
#define LEGEND_ROW_FOUR 29
#define LEGEND_ROW_FIVE 30
#define LEGEND_ROW_SIX 31
#define LEGEND_ROW_SEVEN 33
#define LEGEND_ROW_EIGHT 34
#define LEGEND_ROW_NINE 35

#define INPUT_SLEEP_JIFFIES 1
#define TARGET_CORE_RETRY_SLEEP 1
#define FINISH_WAIT_BUDGET 100000

#define DEMOTION_BURST_LOOPS 2000
#define RUN_BURSTS_BEFORE_SLEEP_LEVEL_ZERO 15
#define RUN_BURSTS_BEFORE_SLEEP_LEVEL_ONE 12
#define RUN_BURSTS_BEFORE_SLEEP_LEVEL_TWO 10
#define LEVEL_ZERO_SLEEP_JIFFIES 2
#define LEVEL_ONE_SLEEP_JIFFIES 1
#define LEVEL_TWO_SLEEP_JIFFIES 1

#define WORKER_ARG_MAGIC 0x4C42564C
#define CORE_UNASSIGNED -1

enum VisualLocationKind {
  VISUAL_HIDDEN = 0,
  VISUAL_GLOBAL = 1,
  VISUAL_LOCAL = 2,
  VISUAL_SLEEP = 3
};

struct LoadWorkerArg {
  int magic;
  int worker_id;
  char token;
  int color;
  int target_level;
  int desired_core;
  int run_bursts_before_sleep;
  int sleep_jiffies;
  int pin_complete;
  int initial_stagger_jiffies;
  int show_global_on_resume;
};

struct WorkerDisplayState {
  int active;
  int token;
  int color;
  int location_kind;
  int location_core;
  int mlfq_level;
};

struct QueueSnapshot {
  char token[MLFQ_LEVELS][MAX_SNAPSHOT_SLOTS];
  int color[MLFQ_LEVELS][MAX_SNAPSHOT_SLOTS];
  int count[MLFQ_LEVELS];
};

static int stop_visual = 0;
static int finished_threads = 0;
static int pinned_workers_ready = 0;
static struct WorkerDisplayState worker_display[TOTAL_WORKERS];
static struct QueueSnapshot sleep_render_snapshot;
static struct QueueSnapshot global_render_snapshot;
static struct QueueSnapshot local_render_snapshot[MAX_CORES];

static void load_balance_worker(void* raw_arg);
static enum ThreadPriority worker_priority(int worker_id);
static int worker_target_level(int worker_id);

// Write one tile directly using the already-loaded text tileset.
static void put_tile_at(int x, int y, char c, int color) {
  TILE_FB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | c);
}

// Draw a short string in tile space.
static void draw_text(int x, int y, char* text, int color) {
  int i = 0;
  while (text[i] != '\0') {
    put_tile_at(x + i, y, text[i], color);
    i++;
  }
}

// Duplicate a token horizontally so queue occupancy is easier to see at a glance.
static void draw_token_cell(int x, int y, int token, int color) {
  put_tile_at(x, y, token, color);
  put_tile_at(x + 1, y, token, color);
}

// Convert a small integer to one hexadecimal digit.
static int hex_digit(unsigned value) {
  if (value < 10) return '0' + value;
  return 'A' + (value - 10);
}

// Border/text color used by the existing priority visual.
static int priority_color(enum ThreadPriority priority) {
  if (priority == HIGH_PRIORITY) return 0x1D;
  if (priority == NORMAL_PRIORITY) return 0xFC;
  return 0xE8;
}

// Register one worker with a stable token/color pair before the thread starts.
static void display_register_worker(struct LoadWorkerArg* arg) {
  struct WorkerDisplayState* state;

  state = &worker_display[arg->worker_id];
  state->token = arg->token;
  state->color = arg->color;
  state->location_kind = VISUAL_HIDDEN;
  state->location_core = CORE_UNASSIGNED;
  state->mlfq_level = LEVEL_ZERO;
  __atomic_store_n(&state->active, 1);
}

// Update the approximate box and MLFQ row used to visualize one worker.
static void display_update_worker(struct LoadWorkerArg* arg,
                                  int location_kind,
                                  int location_core,
                                  int mlfq_level) {
  struct WorkerDisplayState* state;

  state = &worker_display[arg->worker_id];
  __atomic_store_n(&state->location_kind, location_kind);
  __atomic_store_n(&state->location_core, location_core);
  __atomic_store_n(&state->mlfq_level, mlfq_level);
}

// Record that one worker is sleeping or otherwise blocked outside the ready queues.
static void display_sleep_worker(struct LoadWorkerArg* arg, int mlfq_level) {
  display_update_worker(arg, VISUAL_SLEEP, CORE_UNASSIGNED, mlfq_level);
}

// Record that one ANY_CORE worker has just woken and is re-entering the shared
// runnable path before it settles into one core's local queue again.
static void display_global_worker(struct LoadWorkerArg* arg, int mlfq_level) {
  display_update_worker(arg, VISUAL_GLOBAL, CORE_UNASSIGNED, mlfq_level);
}

// Read the current thread's MLFQ level with the documented per-core preconditions.
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

// Return the x coordinate of one local queue box.
static int local_box_x(int core_id) {
  return core_id * LOCAL_BOX_WIDTH;
}

// Clear the token area of one queue box.
static void clear_queue_rows(int x, int y, int visible_slots) {
  int level;
  int slot;

  for (level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    for (slot = 0; slot < visible_slots; slot++) {
      draw_token_cell(x + 3 + (slot * SLOT_WIDTH), y + 2 + level, '.', 0x92);
    }
  }
}

// Draw one queue-box frame and its 0/1/2 MLFQ row labels.
static void draw_queue_box_frame(int x, int y, int width, char* header, int border_color,
                                 int visible_slots) {
  int row;
  int col;

  put_tile_at(x, y, '+', border_color);
  put_tile_at(x + width - 1, y, '+', border_color);
  put_tile_at(x, y + BOX_HEIGHT - 1, '+', border_color);
  put_tile_at(x + width - 1, y + BOX_HEIGHT - 1, '+', border_color);

  for (col = x + 1; col < x + width - 1; col++) {
    put_tile_at(col, y, '-', border_color);
    put_tile_at(col, y + BOX_HEIGHT - 1, '-', border_color);
  }

  for (row = y + 1; row < y + BOX_HEIGHT - 1; row++) {
    put_tile_at(x, row, '|', border_color);
    put_tile_at(x + width - 1, row, '|', border_color);
  }

  draw_text(x + 2, y + 1, header, border_color);

  for (row = LEVEL_ZERO; row <= LEVEL_TWO; row++) {
    put_tile_at(x + 1, y + 2 + row, '0' + row, border_color);
    put_tile_at(x + 2, y + 2 + row, ':', border_color);
  }

  clear_queue_rows(x, y, visible_slots);
}

// Start a snapshot with empty rows so rendering can happen after synchronization ends.
static void snapshot_init(struct QueueSnapshot* snapshot) {
  int level;
  int slot;

  for (level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    snapshot->count[level] = 0;
    for (slot = 0; slot < MAX_SNAPSHOT_SLOTS; slot++) {
      snapshot->token[level][slot] = '.';
      snapshot->color[level][slot] = 0x92;
    }
  }
}

// Add one token to one snapshot row, keeping overflow obvious.
static void snapshot_add(struct QueueSnapshot* snapshot,
                         enum MLFQ_LEVEL level,
                         int token,
                         int color) {
  int index;

  index = snapshot->count[level];
  if (index < MAX_SNAPSHOT_SLOTS) {
    snapshot->token[level][index] = token;
    snapshot->color[level][index] = color;
    snapshot->count[level]++;
    return;
  }

  snapshot->token[level][MAX_SNAPSHOT_SLOTS - 1] = '+';
  snapshot->color[level][MAX_SNAPSHOT_SLOTS - 1] = 0xE8;
}

// Paint one snapshot into its box after the synchronized walk is complete.
static void render_snapshot(int x, int y, int visible_slots, struct QueueSnapshot* snapshot) {
  int level;
  int slot;

  for (level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    for (slot = 0; slot < visible_slots; slot++) {
      draw_token_cell(x + 3 + (slot * SLOT_WIDTH), y + 2 + level,
                      snapshot->token[level][slot],
                      snapshot->color[level][slot]);
    }
  }
}

// Draw one colored token followed by a space so legends remain readable.
static void draw_legend_token(int x, int y, int token, int color) {
  draw_token_cell(x, y, token, color);
  put_tile_at(x + 2, y, ' ', 0xFC);
}

// Draw a contiguous range of worker tokens in the legend.
static void draw_worker_range(int x, int y, int start_id, int count) {
  int i;
  int token;

  for (i = 0; i < count; i++) {
    token = hex_digit(start_id + i);
    draw_legend_token(x + (i * 3), y, token,
                      priority_color(worker_priority(start_id + i)));
  }
}

// Draw every worker token that targets one specific MLFQ level.
static void draw_target_level_range(int x, int y, int target_level) {
  int worker_id;
  int column = 0;

  for (worker_id = 0; worker_id < TOTAL_WORKERS; worker_id++) {
    if (worker_target_level(worker_id) != target_level) continue;
    draw_legend_token(x + (column * 3), y,
                      hex_digit(worker_id),
                      priority_color(worker_priority(worker_id)));
    column++;
  }
}

// Draw the explanatory legend under the queue graphics.
static void draw_legend(void) {
  draw_text(0, LEGEND_ROW_ONE, "Legend:", 0x92);
  draw_text(9, LEGEND_ROW_ONE, "HIGH", priority_color(HIGH_PRIORITY));
  draw_text(15, LEGEND_ROW_ONE, "NORMAL", priority_color(NORMAL_PRIORITY));
  draw_text(23, LEGEND_ROW_ONE, "LOW", priority_color(LOW_PRIORITY));
  draw_text(28, LEGEND_ROW_ONE, "Rows 0/1/2 = MLFQ levels", 0x92);

  draw_text(0, LEGEND_ROW_THREE, "Any-core 0-11:", 0x92);
  draw_worker_range(14, LEGEND_ROW_THREE, 0, ANY_CORE_LEGEND_FIRST_ROW);

  draw_text(0, LEGEND_ROW_FOUR, "Any-core 12-23:", 0x92);
  draw_worker_range(15, LEGEND_ROW_FOUR, ANY_CORE_LEGEND_FIRST_ROW,
                    NUM_ANY_WORKERS - ANY_CORE_LEGEND_FIRST_ROW);

  draw_text(0, LEGEND_ROW_FIVE, "Pinned C0:", 0x92);
  draw_worker_range(11, LEGEND_ROW_FIVE, NUM_ANY_WORKERS, NUM_PINNED_PER_CORE);
  draw_text(24, LEGEND_ROW_FIVE, "Pinned C1:", 0x92);
  draw_worker_range(35, LEGEND_ROW_FIVE, NUM_ANY_WORKERS + NUM_PINNED_PER_CORE,
                    NUM_PINNED_PER_CORE);

  draw_text(0, LEGEND_ROW_SIX, "Pinned C2:", 0x92);
  draw_worker_range(11, LEGEND_ROW_SIX, NUM_ANY_WORKERS +
                    (2 * NUM_PINNED_PER_CORE), NUM_PINNED_PER_CORE);
  draw_text(24, LEGEND_ROW_SIX, "Pinned C3:", 0x92);
  draw_worker_range(35, LEGEND_ROW_SIX, NUM_ANY_WORKERS +
                    (3 * NUM_PINNED_PER_CORE), NUM_PINNED_PER_CORE);

  draw_text(0, LEGEND_ROW_SEVEN, "L0 target:", 0x92);
  draw_target_level_range(11, LEGEND_ROW_SEVEN, LEVEL_ZERO);

  draw_text(0, LEGEND_ROW_EIGHT, "L1 target:", 0x92);
  draw_target_level_range(11, LEGEND_ROW_EIGHT, LEVEL_ONE);

  draw_text(0, LEGEND_ROW_NINE, "L2 target:", 0x92);
  draw_target_level_range(11, LEGEND_ROW_NINE, LEVEL_TWO);
}

// Spend a tiny amount of CPU time between queue transitions so tokens are not
// only visible at wakeup boundaries.
static void worker_burst(void) {
  int i;
  for (i = 0; i < DEMOTION_BURST_LOOPS; i++);
}

// Once a worker has reached its target MLFQ row, let it stay runnable for
// several bursts before it blocks again so the queue view spends less time
// dominated by the sleep box.
static int worker_run_bursts_before_sleep(int target_level) {
  if (target_level == LEVEL_ZERO) return RUN_BURSTS_BEFORE_SLEEP_LEVEL_ZERO;
  if (target_level == LEVEL_ONE) return RUN_BURSTS_BEFORE_SLEEP_LEVEL_ONE;
  return RUN_BURSTS_BEFORE_SLEEP_LEVEL_TWO;
}

// Spend a longer active phase on the current core before the next sleep.
// This keeps workers visible in the runnable boxes for longer without changing
// the scheduler's queueing rules.
static void worker_run_phase(struct LoadWorkerArg* arg) {
  int burst;
  int current_level;

  for (burst = 0;
       burst < arg->run_bursts_before_sleep && __atomic_load_n(&stop_visual) == 0;
       burst++) {
    current_level = read_current_level();
    display_update_worker(arg, VISUAL_LOCAL, get_core_id(), current_level);
    worker_burst();
  }
}

// If this worker is meant to be pinned, keep sleeping until it lands on the
// target core, then pin there exactly once.
static int maybe_pin_worker(struct LoadWorkerArg* arg) {
  if (arg->desired_core == CORE_UNASSIGNED) return 1;
  if (arg->pin_complete) return 1;

  if ((int)get_core_id() == arg->desired_core) {
    core_pin();
    arg->pin_complete = 1;
    __atomic_fetch_add(&pinned_workers_ready, 1);
    return 1;
  }

  display_sleep_worker(arg, read_current_level());
  sleep(TARGET_CORE_RETRY_SLEEP);
  return 0;
}

// Move toward the target MLFQ level using voluntary yields, then mark the
// approximate queue the worker is about to re-enter next. Sleeping threads move
// into the dedicated sleep/block box so they do not look runnable.
static void load_balance_worker(void* raw_arg) {
  struct LoadWorkerArg* arg;
  int current_level = LEVEL_ZERO;

  arg = (struct LoadWorkerArg*)raw_arg;
  assert(arg->magic == WORKER_ARG_MAGIC,
         "thread_load_balance_visual: worker arg magic mismatch.\n");

  if (arg->initial_stagger_jiffies > 0) {
    display_sleep_worker(arg, LEVEL_ZERO);
    sleep(arg->initial_stagger_jiffies);
  }

  while (__atomic_load_n(&stop_visual) == 0) {
    if (!maybe_pin_worker(arg)) continue;

    current_level = read_current_level();

    if (arg->show_global_on_resume) {
      display_global_worker(arg, current_level);
      arg->show_global_on_resume = 0;
      yield();
      continue;
    }

    display_update_worker(arg, VISUAL_LOCAL, get_core_id(), current_level);

    if (current_level < arg->target_level) {
      worker_burst();
      display_update_worker(arg, VISUAL_LOCAL, get_core_id(), current_level);
      yield();
    } else {
      worker_run_phase(arg);
      current_level = read_current_level();
      display_sleep_worker(arg, current_level);
      if (arg->desired_core == CORE_UNASSIGNED) {
        arg->show_global_on_resume = 1;
      }
      sleep(arg->sleep_jiffies);
    }
  }

  display_update_worker(arg, VISUAL_HIDDEN, CORE_UNASSIGNED, current_level);
  __atomic_fetch_add(&finished_threads, 1);
}

// Build the sleep/global/local boxes from the mixed live/test display state.
static void render_worker_display(void) {
  int i;

  snapshot_init(&global_render_snapshot);
  snapshot_init(&sleep_render_snapshot);
  for (i = 0; i < MAX_CORES; i++) {
    snapshot_init(&local_render_snapshot[i]);
  }

  for (i = 0; i < TOTAL_WORKERS; i++) {
    struct WorkerDisplayState* state = &worker_display[i];
    int active;
    int level;
    int location_kind;
    int location_core;

    active = __atomic_load_n(&state->active);
    if (active == 0) continue;

    level = __atomic_load_n(&state->mlfq_level);
    if (level < LEVEL_ZERO || level > LEVEL_TWO) continue;

    location_kind = __atomic_load_n(&state->location_kind);
    location_core = __atomic_load_n(&state->location_core);

    if (location_kind == VISUAL_GLOBAL) {
      snapshot_add(&global_render_snapshot, level, state->token, state->color);
    } else if (location_kind == VISUAL_SLEEP) {
      snapshot_add(&sleep_render_snapshot, level, state->token, state->color);
    } else if (location_kind == VISUAL_LOCAL &&
               location_core >= 0 &&
               location_core < MAX_CORES) {
      snapshot_add(&local_render_snapshot[location_core], level,
                   state->token, state->color);
    }
  }

  render_snapshot(SLEEP_BOX_X, SLEEP_BOX_Y, SLEEP_VISIBLE_SLOTS, &sleep_render_snapshot);
  render_snapshot(GLOBAL_BOX_X, GLOBAL_BOX_Y, GLOBAL_VISIBLE_SLOTS, &global_render_snapshot);
  for (i = 0; i < MAX_CORES; i++) {
    render_snapshot(local_box_x(i), LOCAL_BOX_Y, LOCAL_VISIBLE_SLOTS, &local_render_snapshot[i]);
  }
}

// The ANY_CORE workers are split evenly into HIGH, NORMAL, and LOW groups.
// After that, each core gets one pinned HIGH/NORMAL/LOW worker.
static enum ThreadPriority worker_priority(int worker_id) {
  if (worker_id < NUM_ANY_WORKERS) {
    if (worker_id < NUM_ANY_WORKERS_PER_PRIORITY) return HIGH_PRIORITY;
    if (worker_id < (2 * NUM_ANY_WORKERS_PER_PRIORITY)) return NORMAL_PRIORITY;
    return LOW_PRIORITY;
  }

  if (((worker_id - NUM_ANY_WORKERS) % 3) == 0) return HIGH_PRIORITY;
  if (((worker_id - NUM_ANY_WORKERS) % 3) == 1) return NORMAL_PRIORITY;
  return LOW_PRIORITY;
}

// Target row is derived from token id so the legend remains simple.
static int worker_target_level(int worker_id) {
  return worker_id % 3;
}

// Each target MLFQ level uses a distinct sleep time so the three rows do not
// collapse into identical motion.
static int worker_sleep_jiffies(int target_level) {
  if (target_level == LEVEL_ZERO) return LEVEL_ZERO_SLEEP_JIFFIES;
  if (target_level == LEVEL_ONE) return LEVEL_ONE_SLEEP_JIFFIES;
  return LEVEL_TWO_SLEEP_JIFFIES;
}

// Create one worker token with a stable id, priority color, target row, and
// optional desired core for later pinning.
static void spawn_worker(int worker_id, int desired_core) {
  struct LoadWorkerArg* arg;
  struct Fun* fun;
  enum ThreadPriority priority;
  int target_level;

  priority = worker_priority(worker_id);
  target_level = worker_target_level(worker_id);

  arg = malloc(sizeof(struct LoadWorkerArg));
  assert(arg != NULL,
         "thread_load_balance_visual: LoadWorkerArg allocation failed.\n");
  arg->magic = WORKER_ARG_MAGIC;
  arg->worker_id = worker_id;
  arg->token = hex_digit(worker_id);
  arg->color = priority_color(priority);
  arg->target_level = target_level;
  arg->desired_core = desired_core;
  arg->run_bursts_before_sleep = worker_run_bursts_before_sleep(target_level);
  arg->sleep_jiffies = worker_sleep_jiffies(target_level);
  arg->pin_complete = 0;
  arg->initial_stagger_jiffies = worker_id % 5;
  arg->show_global_on_resume = (desired_core == CORE_UNASSIGNED);
  display_register_worker(arg);

  fun = malloc(sizeof(struct Fun));
  assert(fun != NULL,
         "thread_load_balance_visual: worker Fun allocation failed.\n");
  fun->func = load_balance_worker;
  fun->arg = arg;

  thread_(fun, priority, desired_core);
}

// Draw the full-width sleep/global boxes and the four local queue boxes.
static void draw_all_boxes(void) {
  int core;
  char local_header[8];

  draw_queue_box_frame(SLEEP_BOX_X, SLEEP_BOX_Y, SLEEP_BOX_WIDTH,
                       "SLEEP/BLOCK", 0xF0, SLEEP_VISIBLE_SLOTS);
  draw_queue_box_frame(GLOBAL_BOX_X, GLOBAL_BOX_Y, GLOBAL_BOX_WIDTH,
                       "GLOBAL", 0x1F, GLOBAL_VISIBLE_SLOTS);

  for (core = 0; core < MAX_CORES; core++) {
    local_header[0] = 'C';
    local_header[1] = '0' + core;
    local_header[2] = ' ';
    local_header[3] = 'L';
    local_header[4] = 'O';
    local_header[5] = 'C';
    local_header[6] = '\0';

    draw_queue_box_frame(local_box_x(core), LOCAL_BOX_Y, LOCAL_BOX_WIDTH,
                         local_header,
                         core < (int)CONFIG.num_cores ? 0xAF : 0x92,
                         LOCAL_VISIBLE_SLOTS);
  }
}

int kernel_main(void) {
  int i;
  int wait_count;
  int key;
  int total_threads;

  assert(NUM_PINNED_WORKERS == (MAX_CORES * NUM_PINNED_PER_CORE),
         "load_balance_test: NUM_PINNED_WORKERS must match MAX_CORES * NUM_PINNED_PER_CORE.\n");
  assert((NUM_ANY_WORKERS % PRIORITY_LEVELS) == 0,
         "load_balance_test: NUM_ANY_WORKERS must divide evenly across priorities.\n");
  assert(TOTAL_WORKERS <= 36,
         "load_balance_test: token encoding only supports 36 workers.\n");

  load_text_tiles();
  clear_screen();

  say("Load-balance visual\n", NULL);
  say("GLOBAL = shared-path approximation\n", NULL);
  say("C0..C3 = approximate local state   SLEEP/BLOCK = blocked state\n", NULL);
  say("Rows 0/1/2 = MLFQ levels   Colors = static priority   q = exit\n", NULL);

  draw_all_boxes();
  draw_legend();

  set_priority(LOW_PRIORITY);

  for (i = 0; i < NUM_PINNED_WORKERS; i++) {
    spawn_worker(NUM_ANY_WORKERS + i, i / NUM_PINNED_PER_CORE);
  }

  while (__atomic_load_n(&pinned_workers_ready) != NUM_PINNED_WORKERS) {
    yield();
  }

  for (i = 0; i < NUM_ANY_WORKERS; i++) {
    spawn_worker(i, CORE_UNASSIGNED);
  }

  core_pin();
  set_priority(HIGH_PRIORITY);

  total_threads = TOTAL_WORKERS;

  while (true) {
    render_worker_display();
    key = getkey();
    if (key == 'q' || key == 'Q') break;
  }

  __atomic_store_n(&stop_visual, 1);

  for (wait_count = 0;
       wait_count < FINISH_WAIT_BUDGET &&
       __atomic_load_n(&finished_threads) != total_threads;
       wait_count++) {
    yield();
  }

  return 0;
}
