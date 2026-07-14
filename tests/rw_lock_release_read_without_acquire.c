/*
 * Reader-writer lock read-release-without-acquire negative test.
 *
 * Validates:
 * - rw_lock_release_read() rejects releasing a read side that has no active
 *   readers.
 *
 * How:
 * - initialize an RwLock but intentionally do not acquire a read slot
 * - call rw_lock_release_read(); the expected result is a kernel panic while
 *   the internal lock still protects the reader count
 */

#include "../kernel/rw_lock.h"
#include "../kernel/print.h"

static struct RwLock rwlock;

void kernel_main(void) {
  say("***rw lock read release negative start\n", NULL);

  rw_lock_init(&rwlock);
  rw_lock_release_read(&rwlock);

  say("***rw lock read release negative FAIL\n", NULL);
}
