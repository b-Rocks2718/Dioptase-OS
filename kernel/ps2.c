#include "ps2.h"

static short* ps2_in = (short*)0x7FE5800;

short getkey(void){
  return *ps2_in;
}
