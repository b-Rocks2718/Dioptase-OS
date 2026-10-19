/*
 * Pixel framebuffer test (get_pixel_fb, trap 62, and set_pixel_scale, trap 63):
 * - the getter succeeds and a repeated call returns the same pointer
 * - stores to the first pixel, the first pixel of the second page, and the
 *   last mapped pixel (PIXEL_FB_USER_PIXELS - 1) read back
 * - a forked child sees the parent's pixels at the same address, and the
 *   parent sees the child's store after wait_child(): the mapping aliases the
 *   live framebuffer pages rather than private copies made at fork
 * - a child that loads the first pixel past the mapping is killed. That pixel
 *   shares page 0x7FE5000 with the PS/2, UART, PIT, SD DMA, and audio
 *   registers, so a successful load would mean user code can reach devices.
 * - set_pixel_scroll() (trap 65) returns 0 for positive and negative offsets,
 *   and write_pixel_tail() (trap 66) accepts the kernel-only tail (the first
 *   and last tail pixels) but rejects ranges that start in the user window or
 *   run past the end of the layer with -1, so the trap cannot reach user
 *   memory-mapped devices or write past the framebuffer
 * - set_pixel_scale() returns 0, and a call from the live foreground child is
 *   reported as a display claim when the parent clears the foreground slot,
 *   which is what makes the shell ask the terminal to restore the scale.
 *
 * The faulting child is forked straight after the first getter call, before
 * this process maps anything else (printing may allocate), so in the child's
 * address space the page after the framebuffer window is unmapped and the load
 * must fault instead of landing in some later mapping.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdbool.h"
#include "../../../root/crt/stddef.h"
#include "../../user_test.h"

#define CHILD_STATUS_OK 42
#define CHILD_STATUS_GETTER_CHANGED 51
#define CHILD_STATUS_PARENT_PIXEL_MISSING 52
#define CHILD_STATUS_LOAD_SUCCEEDED 53
#define CHILD_STATUS_GATE_FAILED 54
#define CLOSED_GATE_COUNT 0

// Load the first pixel after the user window. Returning at all is the bug.
static int load_past_window(volatile short* pixel_fb){
  short value = pixel_fb[PIXEL_FB_USER_PIXELS];
  return CHILD_STATUS_LOAD_SUCCEEDED + (value & 0);
}

// Check the inherited mapping without printing, then store a pixel the parent
// must see through its own mapping.
static int child_update_pixel(volatile short* pixel_fb){
  if (get_pixel_fb() != pixel_fb){
    return CHILD_STATUS_GETTER_CHANGED;
  }
  if (pixel_fb[PIXEL_FB_USER_PIXELS - 1] != 0x0789){
    return CHILD_STATUS_PARENT_PIXEL_MISSING;
  }
  pixel_fb[PIXEL_FB_USER_PIXELS - 1] = 0x0ABC;
  return CHILD_STATUS_OK;
}

// Change the pixel scale as the foreground child once the parent has
// installed it, so the kernel attributes the display trap to this child.
static int child_set_pixel_scale(int gate){
  if (sem_down(gate) != 0){
    return CHILD_STATUS_GATE_FAILED;
  }
  set_pixel_scale(0);
  return CHILD_STATUS_OK;
}

// Run one foreground child that changes the pixel scale and report whether
// clearing the foreground slot saw its display claim.
static void test_pixel_scale_claim(void){
  int gate = sem_open(CLOSED_GATE_COUNT);
  int child = fork();
  if (child == 0){
    exit(child_set_pixel_scale(gate));
  }
  user_test_expect_eq("set foreground child", set_foreground_child(child), 0);
  sem_up(gate);
  user_test_expect_eq("pixel scale child exit status", wait_child(child),
    CHILD_STATUS_OK);
  user_test_expect_eq("child's pixel scale change claims the display",
    set_foreground_child(-1), 1);
  sem_close(gate);
}

int main(void){ /* Verify the pixel framebuffer window (bounds, aliasing, idempotence) and the scale trap. */
  volatile short* pixel_fb = get_pixel_fb();
  bool mapped = pixel_fb != NULL && (int)pixel_fb != -1;
  int fault_status = 0;
  if (mapped){
    int fault_child = fork();
    if (fault_child == 0){
      return load_past_window(pixel_fb);
    }
    // Reap it before printing so the kernel's kill report cannot interleave
    // with this process's output lines.
    fault_status = wait_child(fault_child);
  }

  user_test_expect_eq("pixel framebuffer mapping succeeds", mapped, 1);
  if (!mapped){
    return 1;
  }
  user_test_expect_eq("load past the user window kills the child",
    fault_status, -1);
  user_test_expect_eq("repeated getter reuses pointer",
    get_pixel_fb() == pixel_fb, 1);

  pixel_fb[0] = 0x0123;
  pixel_fb[2048] = 0x0456;
  pixel_fb[PIXEL_FB_USER_PIXELS - 1] = 0x0789;
  user_test_expect_eq("first pixel reads back", pixel_fb[0], 0x0123);
  user_test_expect_eq("second page's first pixel reads back", pixel_fb[2048],
    0x0456);
  user_test_expect_eq("last mapped pixel reads back",
    pixel_fb[PIXEL_FB_USER_PIXELS - 1], 0x0789);

  int child = fork();
  if (child == 0){
    return child_update_pixel(pixel_fb);
  }
  user_test_expect_eq("alias child exit status", wait_child(child),
    CHILD_STATUS_OK);
  user_test_expect_eq("parent sees the child's pixel",
    pixel_fb[PIXEL_FB_USER_PIXELS - 1], 0x0ABC);

  short tail_pixels[4] = {0x0111, 0x0222, 0x0333, 0x0444};
  user_test_expect_eq("set_pixel_scroll(3, -2)", set_pixel_scroll(3, -2), 0);
  user_test_expect_eq("set_pixel_scroll(0, 0)", set_pixel_scroll(0, 0), 0);
  user_test_expect_eq("write_pixel_tail at the first tail pixel",
    write_pixel_tail(PIXEL_FB_USER_PIXELS, tail_pixels, 4), 0);
  user_test_expect_eq("write_pixel_tail at the last tail pixel",
    write_pixel_tail(PIXEL_FB_PIXELS - 1, tail_pixels, 1), 0);
  user_test_expect_eq("write_pixel_tail rejects a start in the user window",
    write_pixel_tail(PIXEL_FB_USER_PIXELS - 1, tail_pixels, 2), -1);
  user_test_expect_eq("write_pixel_tail rejects a range past the layer",
    write_pixel_tail(PIXEL_FB_PIXELS - 1, tail_pixels, 2), -1);
  user_test_expect_eq("set_pixel_scale(1)", set_pixel_scale(1), 0);
  user_test_expect_eq("set_pixel_scale(0)", set_pixel_scale(0), 0);
  test_pixel_scale_claim();
  return 0;
}
