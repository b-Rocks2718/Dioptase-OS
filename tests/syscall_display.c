/*
 * syscall_display:
 * - exercise trap-dispatcher syscalls that update MMIO-backed VGA state or
 *   kernel-side console state without needing user pointers
 * - verify request_priority() validates user priority values and updates the
 *   current TCB's static priority
 * - verify the values they expose stay within the documented hardware contract
 * - stress whole-buffer console serialization across four worker threads and
 *   across the software framebuffer-wrap boundary
 * - verify a full screen of continuous text scrolls before reusing the top row
 * - verify console ownership preserves an already-disabled IMR and preemption
 *   state exactly
 * - smoke-test trap sleep/yield on the current kernel thread and ensure a
 *   duration outside the modular half-range is rejected without blocking
 */

#include "../kernel/config.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/interrupts.h"
#include "../kernel/machine.h"
#include "../kernel/semaphore.h"
#include "../kernel/sys.h"
#include "../kernel/threads.h"
#include "../kernel/vga.h"
#include "../kernel/per_core.h"

#define TEST_TILE_SCALE 3
#define TEST_VSCROLL 17
#define TEST_HSCROLL -11
#define TEST_TEXT_COLOR 0x5A
#define TRANSPARENT_TILE_PIXEL_OFFSET 16320
#define CONSOLE_STRESS_WORKERS 4
#define CONSOLE_STRESS_BYTES_PER_WORKER 1536
#define CONSOLE_STRESS_TILE_BASE 'A'
#define CONSOLE_STRESS_FINAL_CURSOR 1344 // 6,144 bytes modulo 4,800 framebuffer entries.
#define CONSOLE_STRESS_FIRST_VISIBLE_RUN 192 // The first transaction's visible tail.
#define TILE_INDEX_MASK 0xFFu // Tile framebuffer entries store the tile index in the low byte.
#define TILE_ROW_HEIGHT_PIXELS 8
// BCC currently requires one literal token for this file-scope array bound.
// 4,801 is the documented 4,800-tile framebuffer plus the first reused tile.
#define CONTINUOUS_WRAP_BYTES 4801
#define CONTINUOUS_WRAP_OLD_TILE 'W'
#define CONTINUOUS_WRAP_NEW_TILE 'X'

static struct Semaphore console_stress_start;
static struct Semaphore console_stress_done;
static char console_stress_buffers[CONSOLE_STRESS_WORKERS]
  [CONSOLE_STRESS_BYTES_PER_WORKER];
static char continuous_wrap_buffer[CONTINUOUS_WRAP_BYTES];

struct ConsoleStressArg {
  int worker;
};

extern int trap_handler(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
    bool* return_to_user);

static void emit_result(char* name, int value){
  void* args[2];

  args[0] = name;
  args[1] = (void*)value;
  say("***syscall_display %s = %d\n", args);
}

static int call_trap(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7){
  bool return_to_user = false;
  int rc = trap_handler(code, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
    &return_to_user);

  if (!return_to_user){
    panic("syscall_display: trap unexpectedly refused to return to caller\n");
  }

  return rc;
}

/*
 * One worker owns its full counted buffer as a single console transaction.
 * Four buffers total 6,144 characters, so a correct run necessarily crosses
 * the 4,800-entry framebuffer boundary while other cores contend for the same
 * cursor/MMIO state. The completion semaphore is signaled only after the
 * worker has released console ownership.
 */
static void console_stress_worker(void* raw_arg){
  struct ConsoleStressArg* arg = (struct ConsoleStressArg*)raw_arg;
  sem_down(&console_stress_start);
  console_write(console_stress_buffers[arg->worker],
    CONSOLE_STRESS_BYTES_PER_WORKER);
  sem_up(&console_stress_done);
}

static enum CoreAffinity console_stress_affinity(int worker){
  if (worker == 0) return CORE_0;
  if (worker == 1) return CORE_1;
  if (worker == 2) return CORE_2;
  return CORE_3;
}

static int run_console_stress(void){
  bool saved_use_vga = CONFIG.use_vga;
  CONFIG.use_vga = true;
  clear_screen();
  sem_init(&console_stress_start, 0);
  sem_init(&console_stress_done, 0);

  for (int worker = 0; worker < CONSOLE_STRESS_WORKERS; ++worker){
    for (int i = 0; i < CONSOLE_STRESS_BYTES_PER_WORKER; ++i){
      console_stress_buffers[worker][i] = CONSOLE_STRESS_TILE_BASE + worker;
    }

    struct ConsoleStressArg* arg = malloc(sizeof(struct ConsoleStressArg));
    assert(arg != NULL,
      "syscall_display console stress: worker argument allocation failed.\n");
    arg->worker = worker;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL,
      "syscall_display console stress: worker function allocation failed.\n");
    fun->func = console_stress_worker;
    fun->arg = arg;

    enum CoreAffinity affinity = ANY_CORE;
    if (worker < (int)CONFIG.num_cores){
      affinity = console_stress_affinity(worker);
    }
    thread_(fun, NORMAL_PRIORITY, affinity);
  }

  // No worker can enter console_write() until all four TCBs exist. Releasing
  // the four permits together makes the pinned workers contend on the console
  // from their respective cores instead of letting the creation loop serialize
  // them accidentally.
  for (int worker = 0; worker < CONSOLE_STRESS_WORKERS; ++worker){
    sem_up(&console_stress_start);
  }

  for (int worker = 0; worker < CONSOLE_STRESS_WORKERS; ++worker){
    sem_down(&console_stress_done);
  }
  sem_destroy(&console_stress_start);
  sem_destroy(&console_stress_done);

  /*
   * Console ownership may choose any worker ordering. In that ordering, the
   * last 4,800 bytes consist of 192 bytes from the first transaction followed
   * by three complete 1,536-byte transactions. Starting at the final cursor
   * therefore exposes four uniform runs whose tile IDs are all distinct. This
   * accepts every legal lock acquisition order but rejects character-level
   * interleaving, lost cursor updates, and an incorrect wrap boundary.
   *
   * This test-only read intentionally bypasses the console lock after every
   * worker has completed; no kernel-owned writer remains active at this point.
   */
  int valid = 1;
  unsigned seen_tiles = 0;
  int logical_offset = 0;
  for (int run = 0; run < CONSOLE_STRESS_WORKERS && valid; ++run){
    int run_length = CONSOLE_STRESS_BYTES_PER_WORKER;
    if (run == 0){
      run_length = CONSOLE_STRESS_FIRST_VISIBLE_RUN;
    }

    int framebuffer_index =
      (CONSOLE_STRESS_FINAL_CURSOR + logical_offset) % FB_NUM_TILES;
    unsigned tile =
      (unsigned short)TILE_FB[framebuffer_index] & TILE_INDEX_MASK;
    if (tile < CONSOLE_STRESS_TILE_BASE ||
        tile >= CONSOLE_STRESS_TILE_BASE + CONSOLE_STRESS_WORKERS){
      valid = 0;
      break;
    }

    unsigned tile_bit = 1u << (tile - CONSOLE_STRESS_TILE_BASE);
    if ((seen_tiles & tile_bit) != 0){
      valid = 0;
      break;
    }
    seen_tiles |= tile_bit;

    for (int i = 0; i < run_length; ++i){
      framebuffer_index =
        (CONSOLE_STRESS_FINAL_CURSOR + logical_offset + i) % FB_NUM_TILES;
      unsigned observed =
        (unsigned short)TILE_FB[framebuffer_index] & TILE_INDEX_MASK;
      if (observed != tile){
        valid = 0;
        break;
      }
    }
    logical_offset += run_length;
  }
  valid = valid && logical_offset == FB_NUM_TILES &&
    seen_tiles == ((1u << CONSOLE_STRESS_WORKERS) - 1);

  clear_screen();
  CONFIG.use_vga = saved_use_vga;
  return valid;
}

/*
 * Fill the circular text framebuffer without any newline, then emit one more
 * character. The extra character must enter a cleared top row and move the
 * signed vertical-scroll register by one 8-pixel text row. Before the repair,
 * only a newline armed this transition, so the extra byte overwrote TILE_FB[0]
 * while the rest of the old top row stayed visible and VSCROLL did not move.
 */
static int console_continuous_wrap_scrolls(void){
  bool saved_use_vga = CONFIG.use_vga;
  short saved_vscroll = *TILE_VSCROLL;
  CONFIG.use_vga = true;

  clear_screen();
  console_set_tile_vscroll(0);
  for (int i = 0; i < FB_NUM_TILES; ++i){
    continuous_wrap_buffer[i] = CONTINUOUS_WRAP_OLD_TILE;
  }
  continuous_wrap_buffer[FB_NUM_TILES] = CONTINUOUS_WRAP_NEW_TILE;
  console_write(continuous_wrap_buffer, CONTINUOUS_WRAP_BYTES);

  int valid = *TILE_VSCROLL == -TILE_ROW_HEIGHT_PIXELS &&
    (((unsigned short)TILE_FB[0]) & TILE_INDEX_MASK) ==
      CONTINUOUS_WRAP_NEW_TILE &&
    TILE_FB[1] == 0 && TILE_FB[TILE_ROW_WIDTH - 1] == 0 &&
    (((unsigned short)TILE_FB[TILE_ROW_WIDTH]) & TILE_INDEX_MASK) ==
      CONTINUOUS_WRAP_OLD_TILE;

  clear_screen();
  console_set_tile_vscroll(saved_vscroll);
  CONFIG.use_vga = saved_use_vga;
  return valid;
}

/*
 * Exercise the reentrant lock's outer path with both scheduling mechanisms
 * already disabled. The console call must restore the exact device-bit mask,
 * not merely the global interrupt-enable bit, and must leave can_preempt false.
 */
static int console_preserves_disabled_cpu_state(void){
  unsigned original_imr = interrupts_disable();
  unsigned disabled_imr = original_imr & ~GLOBAL_INT_ENABLE;
  interrupts_restore(disabled_imr);

  bool original_preemption = preemption_disable();
  char empty_buffer[1] = { 0 };
  console_write(empty_buffer, 0);

  bool preserved_by_call =
    get_imr() == disabled_imr && !get_current_tcb()->can_preempt;

  preemption_restore(original_preemption);
  interrupts_restore(original_imr);

  bool restored_after_probe =
    get_imr() == original_imr &&
    get_current_tcb()->can_preempt == original_preemption;
  return preserved_by_call && restored_after_probe;
}

int kernel_main(void){
  unsigned before_jiffies = call_trap(TRAP_GET_CURRENT_JIFFIES, 0, 0, 0, 0, 0,
    0, 0);
  unsigned before_frame = call_trap(TRAP_GET_VGA_FRAME_COUNTER, 0, 0, 0, 0, 0,
    0, 0);

  emit_result("getkey", call_trap(TRAP_GET_KEY, 0, 0, 0, 0, 0, 0, 0));

  emit_result("sleep_rejects_high_bit_duration",
    call_trap(TRAP_SLEEP, -1, 0, 0, 0, 0, 0, 0));
  call_trap(TRAP_SLEEP, 1, 0, 0, 0, 0, 0, 0);
  emit_result("sleep_elapsed",
    call_trap(TRAP_GET_CURRENT_JIFFIES, 0, 0, 0, 0, 0, 0, 0) >=
    before_jiffies + 1);

  emit_result("yield", call_trap(TRAP_YIELD, 0, 0, 0, 0, 0, 0, 0));

  emit_result("request_priority_invalid_low",
    call_trap(TRAP_REQUEST_PRIORITY, -1, 0, 0, 0, 0, 0, 0));
  emit_result("priority_after_invalid_low", get_current_tcb()->priority);

  emit_result("request_priority_low",
    call_trap(TRAP_REQUEST_PRIORITY, LOW_PRIORITY, 0, 0, 0, 0, 0, 0));
  emit_result("priority_low", get_current_tcb()->priority);

  emit_result("request_priority_high",
    call_trap(TRAP_REQUEST_PRIORITY, HIGH_PRIORITY, 0, 0, 0, 0, 0, 0));
  emit_result("priority_high", get_current_tcb()->priority);

  emit_result("request_priority_invalid_high",
    call_trap(TRAP_REQUEST_PRIORITY, HIGH_PRIORITY + 1, 0, 0, 0, 0, 0, 0));
  emit_result("priority_after_invalid_high", get_current_tcb()->priority);

  emit_result("request_priority_normal",
    call_trap(TRAP_REQUEST_PRIORITY, NORMAL_PRIORITY, 0, 0, 0, 0, 0, 0));
  emit_result("priority_normal", get_current_tcb()->priority);

  call_trap(TRAP_SET_TILE_SCALE, TEST_TILE_SCALE, 0, 0, 0, 0, 0, 0);
  emit_result("tile_scale", *TILE_SCALE);

  call_trap(TRAP_SET_VSCROLL, TEST_VSCROLL, 0, 0, 0, 0, 0, 0);
  emit_result("vscroll", *TILE_VSCROLL);

  call_trap(TRAP_SET_HSCROLL, TEST_HSCROLL, 0, 0, 0, 0, 0, 0);
  emit_result("hscroll", *TILE_HSCROLL);

  call_trap(TRAP_LOAD_TEXT_TILES, 0, 0, 0, 0, 0, 0, 0);
  emit_result("load_text_tiles",
    (unsigned short)TILEMAP[TRANSPARENT_TILE_PIXEL_OFFSET]);

  TILE_FB[0] = 0x1234;
  call_trap(TRAP_CLEAR_SCREEN, 0, 0, 0, 0, 0, 0, 0);
  emit_result("clear_screen", TILE_FB[0]);

  call_trap(TRAP_SET_TEXT_COLOR, TEST_TEXT_COLOR, 0, 0, 0, 0, 0, 0);
  emit_result("text_color", console_get_text_color());

  emit_result("console_preserves_disabled_state",
    console_preserves_disabled_cpu_state());
  emit_result("console_multicore_wrap", run_console_stress());
  emit_result("console_continuous_wrap_scrolls",
    console_continuous_wrap_scrolls());

  emit_result("vga_status_valid",
    (call_trap(TRAP_GET_VGA_STATUS, 0, 0, 0, 0, 0, 0, 0) & ~0x3) == 0);

  call_trap(TRAP_SLEEP, 1, 0, 0, 0, 0, 0, 0);
  emit_result("frame_counter_monotonic",
    call_trap(TRAP_GET_VGA_FRAME_COUNTER, 0, 0, 0, 0, 0, 0, 0) >=
    before_frame);

  return 0;
}
