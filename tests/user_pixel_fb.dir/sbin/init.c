/*
 * Pixel framebuffer getter test (get_pixel_fb, trap 62):
 * - the getter succeeds and a repeated call returns the same pointer
 * - stores to the first pixel, the first pixel of the second page, and the
 *   last mapped pixel (PIXEL_FB_USER_PIXELS - 1) read back
 * - a forked child sees the parent's pixels at the same address, and the
 *   parent sees the child's store after wait_child(): the mapping aliases the
 *   live framebuffer pages rather than private copies made at fork
 * - a child that loads the first pixel past the mapping is killed. That pixel
 *   shares page 0x7FE5000 with the PS/2, UART, PIT, SD DMA, and audio
 *   registers, so a successful load would mean user code can reach devices.
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

int main(void){ /* Verify the pixel framebuffer window: bounds, aliasing, and idempotence. */
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
  return 0;
}
