#include "machine.h"

void pause(void){
  // sleep if it is safe to do so
  //unsigned imr = get_imr();
  //if ((imr & 0x80000000) != 0 && (imr & 1) != 0){ // global enabled and timer enabled
  //  sleep();
  //} // otherwise, just return
}
