/*
 * user_pixel_keyboard guest:
 * - map the pixel framebuffer, write one pixel, and read it back
 * - repeat the map and require the same pointer without printing it
 * - set and restore pixel scale
 * - reject a keyboard mode other than 0 or 1
 * - observe the foreground child's raw-keyboard flag, including the cooked
 *   state a new child starts in
 *
 * The child waits on a semaphore until the parent has installed it as the
 * foreground child, so the flag read does not depend on who is scheduled first.
 * This process maps the display itself; the child does not, so clearing the
 * foreground slot must not report a display claim.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define CLOSED_GATE_COUNT 0
#define CHILD_FAILURE_STATUS 73

static int wait_for_parent(int gate){ /* Block until the parent has published foreground state. */
  if (sem_down(gate) != 0){
    return CHILD_FAILURE_STATUS;
  }
  return 0;
}

static void test_pixel_and_raw_self(void){ /* Check the pixel window and this thread's own keyboard mode. */
  volatile short* pixels = get_pixel_fb();
  volatile short* again = get_pixel_fb();
  int mapped = pixels != (volatile short*)-1;

  user_test_expect_eq("pixel mapped", mapped, 1);
  user_test_expect_eq("pixel idempotent", mapped && again == pixels, 1);
  if (mapped){
    pixels[0] = 0x123;
    user_test_expect_eq("pixel readback", pixels[0], 0x123);
  } else {
    user_test_expect_eq("pixel readback", 0, 0x123);
  }
  user_test_expect_eq("pixel scale", set_pixel_scale(1), 0);
  user_test_expect_eq("pixel scale restore", set_pixel_scale(0), 0);
  user_test_expect_eq("raw invalid", set_keyboard_raw(2), -1);
  user_test_expect_eq("raw on", set_keyboard_raw(1), 0);
  user_test_expect_eq("raw off", set_keyboard_raw(0), 0);
  user_test_expect_eq("no foreground raw", foreground_keyboard_raw(), 0);
}

static void test_foreground_raw_flag(void){ /* A foreground child starts cooked, then its raw flag is visible. */
  int start = sem_open(CLOSED_GATE_COUNT);
  int done = sem_open(CLOSED_GATE_COUNT);
  int child = fork();

  if (child == 0){
    int rc = wait_for_parent(start);
    if (rc != 0){
      exit(rc);
    }
    if (set_keyboard_raw(1) != 0){
      exit(CHILD_FAILURE_STATUS);
    }
    sem_up(done);
    if (sem_down(start) != 0){
      exit(CHILD_FAILURE_STATUS);
    }
    if (set_keyboard_raw(0) != 0){
      exit(CHILD_FAILURE_STATUS);
    }
    sem_up(done);
    exit(0);
  }

  user_test_expect_eq("set foreground", set_foreground_child(child), 0);
  user_test_expect_eq("child starts cooked", foreground_keyboard_raw(), 0);
  sem_up(start);
  sem_down(done);
  user_test_expect_eq("foreground raw on", foreground_keyboard_raw(), 1);
  sem_up(start);
  sem_down(done);
  user_test_expect_eq("foreground raw off", foreground_keyboard_raw(), 0);
  user_test_expect_eq("wait child", wait_child(child), 0);
  user_test_expect_eq("clear foreground", set_foreground_child(-1), 0);
  sem_close(start);
  sem_close(done);
}

int main(void){ /* Verify the pixel framebuffer map and raw keyboard mode. */
  test_pixel_and_raw_self();
  test_foreground_raw_flag();
  return 0;
}
