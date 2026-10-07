#include "../../../root/crt/sys.h"
#include "../../../root/crt/mouse.h"

// Return 0 if every mouse.h field decodes as expected, else the failed check.
static int check_decoding(void){
  // Left+right held, DX = -3, DY = +5, WHEEL = -2.
  int a = (int)0xFE05FD0B;
  if (MOUSE_BUTTONS(a) != (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)) return 1;
  if (MOUSE_DX(a) != -3) return 2;
  if (MOUSE_DY(a) != 5) return 3;
  if (MOUSE_WHEEL(a) != -2) return 4;

  // Middle held, extreme DX = +127, DY = -128, WHEEL = +1.
  int b = 0x01807F0C;
  if (MOUSE_BUTTONS(b) != MOUSE_BUTTON_MIDDLE) return 5;
  if (MOUSE_DX(b) != 127) return 6;
  if (MOUSE_DY(b) != -128) return 7;
  if (MOUSE_WHEEL(b) != 1) return 8;
  return 0;
}

int main(void){ /* Check the getmouse trap and mouse.h decoding from user mode. */
  // Report the trap result so the expected output shows "no event" (0).
  test_syscall(getmouse());

  int failed = check_decoding();
  if (failed != 0) return failed;
  return 42;
}
