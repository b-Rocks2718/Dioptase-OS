/*
 * CLH-lock identity negative test.
 *
 * A TCB has one CLH ticket and predecessor, so the legacy release path could
 * silently release whichever lock that ticket represented even when passed a
 * different CLHLock pointer. Exact per-lock ownership must reject that call.
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static struct CLHLock owned;
static struct CLHLock other;

void kernel_main(void){
  say("***CLH-lock wrong-release negative start\n", NULL);
  clh_lock_init(&owned);
  clh_lock_init(&other);
  clh_lock_acquire(&owned);
  clh_lock_release(&other);
  panic("CLH-lock wrong-release test: release unexpectedly succeeded.\n");
}
