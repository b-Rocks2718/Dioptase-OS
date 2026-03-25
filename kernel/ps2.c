#include "ps2.h"

// PS/2 MMIO address for keyboard input
static short* ps2_in = (short*)0x7FE5800;

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void){
  return *ps2_in;
}
