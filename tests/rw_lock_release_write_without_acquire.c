/*
 * Reader-writer lock write-release-without-acquire negative test.
 *
 * Validates:
 * - rw_lock_release_write() rejects releasing a write side that has no active
 *   writer.
 *
 * How:
 * - initialize an RwLock but intentionally do not acquire the write side
 * - call rw_lock_release_write(); the expected result is a kernel panic while
 *   the internal lock still protects the writer-active flag
 */

#include "../kernel/rw_lock.h"
#include "../kernel/print.h"

static struct RwLock rwlock;

void kernel_main(void) {
  say("***rw lock write release negative start\n", NULL);

  rw_lock_init(&rwlock);
  rw_lock_release_write(&rwlock);

  say("***rw lock write release negative FAIL\n", NULL);
}
