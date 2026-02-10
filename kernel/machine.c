#include "machine.h"
#include "pit.h"

void pause(void){
  // sleep if it is safe to do so
  unsigned imr = get_imr();
  // require global interrupt enable bit, and timer interrupt enabled,
  // and current_jiffies > 0 so we know the pit has been initialized
  if ((imr & 0x80000000) != 0 && (imr & 1) != 0 && current_jiffies != 0){ 
    mode_sleep();
  } // otherwise, just return
}
