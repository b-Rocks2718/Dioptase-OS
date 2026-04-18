/*
 * syscall_display:
 * - exercise trap-dispatcher syscalls that update MMIO-backed VGA state or
 *   kernel-side console state without needing user pointers
 * - verify the values they expose stay within the documented hardware contract
 * - smoke-test trap sleep/yield on the current kernel thread
 */

#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/sys.h"
#include "../kernel/vga.h"

#define TEST_TILE_SCALE 3
#define TEST_VSCROLL 17
#define TEST_HSCROLL -11
#define TEST_TEXT_COLOR 0x5A
#define TRANSPARENT_TILE_PIXEL_OFFSET 16320

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

int kernel_main(void){
  unsigned before_jiffies = call_trap(TRAP_GET_CURRENT_JIFFIES, 0, 0, 0, 0, 0,
    0, 0);
  unsigned before_frame = call_trap(TRAP_GET_VGA_FRAME_COUNTER, 0, 0, 0, 0, 0,
    0, 0);

  emit_result("getkey", call_trap(TRAP_GET_KEY, 0, 0, 0, 0, 0, 0, 0));

  call_trap(TRAP_SLEEP, 1, 0, 0, 0, 0, 0, 0);
  emit_result("sleep_elapsed",
    call_trap(TRAP_GET_CURRENT_JIFFIES, 0, 0, 0, 0, 0, 0, 0) >=
    before_jiffies + 1);

  emit_result("yield", call_trap(TRAP_YIELD, 0, 0, 0, 0, 0, 0, 0));

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
  emit_result("text_color", text_color);

  emit_result("vga_status_valid",
    (call_trap(TRAP_GET_VGA_STATUS, 0, 0, 0, 0, 0, 0, 0) & ~0x3) == 0);

  call_trap(TRAP_SLEEP, 1, 0, 0, 0, 0, 0, 0);
  emit_result("frame_counter_monotonic",
    call_trap(TRAP_GET_VGA_FRAME_COUNTER, 0, 0, 0, 0, 0, 0, 0) >=
    before_frame);

  return 0;
}
