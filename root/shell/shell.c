#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"

#define DEMO_PAUSE 600
#define SCROLL_TEST_LINES 70

static void pause_demo(void){
  sleep(DEMO_PAUSE);
}

static void print_scroll_lines(void){
  puts("\n");
  for (int i = 0; i < SCROLL_TEST_LINES; ++i){
    int args[1] = {i};
    printf("scroll line %d: forcing terminal wrap and vscroll\n", args);
  }
}

int main(void){
  puts("\x1b[2J\x1b[1;1H");
  puts("Dioptase terminal escape sequence demo\n");
  puts("Shows: \\r, \\b, \\t, delayed wrap, H/f, A/B/C/D, J, K, L, M, m, s/u, and ?25h/?25l\n");
  pause_demo();

  puts("\x1b[3;1HColors: ");
  puts("\x1b[31mred ");
  puts("\x1b[32mgreen ");
  puts("\x1b[33myellow ");
  puts("\x1b[34mblue ");
  puts("\x1b[35mmagenta ");
  puts("\x1b[36mcyan ");
  puts("\x1b[37mwhite\x1b[0m");
  pause_demo();

  puts("\x1b[5;1HPlaced with H at row 5, col 1");
  puts("\x1b[6;20fPlaced with f at row 6, col 20");
  pause_demo();

  puts("\x1b[8;10H@");
  pause_demo();
  puts("\x1b[4D<");
  pause_demo();
  puts("\x1b[6C>");
  pause_demo();
  puts("\x1b[1A^");
  pause_demo();
  puts("\x1b[2Bv");
  pause_demo();

  puts("\x1b[12;1HThis line will be partially erased........");
  pause_demo();
  puts("\x1b[12;23H\x1b[K<- K erased to end of line");
  pause_demo();

  puts("\x1b[14;1HL/M demo line 1");
  puts("\x1b[15;1HL/M demo line 2");
  puts("\x1b[16;1HL/M demo line 3");
  pause_demo();
  puts("\x1b[15;1H\x1b[LInserted with L");
  pause_demo();
  puts("\x1b[16;1H\x1b[M");
  puts("\x1b[17;1HDeleted one shifted line with M");
  pause_demo();

  puts("\x1b[19;1HJ demo: next pause clears from row 20 down");
  puts("\x1b[20;1HThis text should disappear.");
  puts("\x1b[21;1HSo should this.");
  pause_demo();
  puts("\x1b[20;1H\x1b[J");
  puts("\x1b[20;1HJ cleared from the cursor to the end of the screen.");
  pause_demo();

  puts("\x1b[22;1Hs/u demo: save cursor, move away, then restore");
  pause_demo();
  puts("\x1b[s");
  puts("\x1b[2B\x1b[20Cafter save");
  pause_demo();
  puts("\x1b[u");
  puts(" <- text emitted after CSI u");
  pause_demo();

  puts("\x1b[25;1H?25l/?25h demo: hide cursor for one pause");
  pause_demo();
  puts("\x1b[?25l");
  pause_demo();
  puts("\x1b[?25h");
  puts("\x1b[29;1HCR demo: start here");
  pause_demo();
  puts("\rCR demo: after \\r");
  pause_demo();

  puts("\x1b[31;1HBS demo: abcde");
  pause_demo();
  puts("\b\bXY");
  pause_demo();

  puts("\x1b[33;1HTabs:\tA\tB\tC");
  pause_demo();

  puts("\x1b[35;1HScroll test: next section forces the terminal to scroll.");
  pause_demo();

  print_scroll_lines();
  pause_demo();

  puts("Post-scroll cursor movement demo");
  puts("\x1b[3D@");
  pause_demo();
  puts("\x1b[4D<");
  pause_demo();
  puts("\x1b[6C>");
  pause_demo();
  puts("\x1b[1A^");
  pause_demo();
  puts("\x1b[2Bv");
  pause_demo();
  puts("\x1b[s");
  puts("\x1b[2B\x1b[12Cafter save while scrolled");
  pause_demo();
  puts("\x1b[u");
  puts(" <- restored while scrolled");
  pause_demo();
  puts("\x1b[?25l");
  pause_demo();
  puts("\x1b[?25h");
  puts("\x1b[56;1H\x1b[JDelayed wrap demo after scroll:");
  puts("\x1b[57;1HThe next pause stops with the bottom-right cell filled.");
  puts("\x1b[58;1HThe following byte triggers the actual wrap and scroll.");
  pause_demo();
  puts("\x1b[60;1H12345678901234567890123456789012345678901234567890123456789012345678901234567890");
  pause_demo();
  puts("! delayed wrap consumed here");
  pause_demo();
  puts("\nDemo complete. Shell is idle.");

  while (true) { sleep(50); }

  return 0;
}
