#include "machine.h"
#include "pit.h"

// Put this core to sleep if it is safe to do so, otherwise just return
void pause(void){
  unsigned imr = get_imr();

  // ASSUMPTION: if an interrupt happens here, it correctly restores imr
  // otherwise we could end up sleeping when it is not safe to do so

  // require global interrupt enable bit, and timer interrupt enabled,
  // and current_jiffies > 0 so we know the pit has been initialized
  if ((imr & 0x80000000) != 0 && (imr & 1) != 0 && current_jiffies != 0){ 
    mode_sleep();
  } // otherwise, just return
}
