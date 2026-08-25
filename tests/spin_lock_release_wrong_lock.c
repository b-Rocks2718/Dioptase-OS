/*
 * Spin-lock identity negative test.
 *
 * The TCB's CLH node records that this thread owns some spin lock, but that is
 * insufficient to authorize releasing a different object. Holding `owned`
 * and passing `other` must panic without clearing either lock's ownership.
 */

#include "../kernel/atomic.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static struct SpinLock owned;
static struct SpinLock other;

void kernel_main(void){
  say("***spin-lock wrong-release negative start\n", NULL);
  spin_lock_init(&owned);
  spin_lock_init(&other);
  spin_lock_acquire(&owned);
  spin_lock_release(&other);
  panic("spin-lock wrong-release test: release unexpectedly succeeded.\n");
}
