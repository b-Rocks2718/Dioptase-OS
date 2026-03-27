#include "ps2.h"
#include "threads.h"
#include "machine.h"

// PS/2 MMIO address for keyboard input
static short* ps2_in = (short*)0x7FE5800;

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void){
  return *ps2_in;
}

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey(void){
  short key = 0;
  while ((key = *ps2_in) == 0) {
    // TODO: actually block until a ps2 interrupt
    yield();
  }
  return key;
}

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey_spin(void){
  short key = 0;
  while ((key = *ps2_in) == 0) {
    // spin until a key is pressed

    // sleep to save power
    pause();
  }
  return key;
}
