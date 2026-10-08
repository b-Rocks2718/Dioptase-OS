#include "watchdog.h"

#include "debug.h"
#include "heap.h"
#include "pit.h"
#include "print.h"
#include "threads.h"

static void (*watchdog_checks[WATCHDOG_MAX_CHECKS])(unsigned now);

// Number of published entries in watchdog_checks. Registration stores the
// entry before publishing the new count, so the daemon only calls initialized
// entries even if it starts sweeping while registration is still running.
static int watchdog_check_count = 0;

/*
 * Persistent daemon: sleep one poll interval, then run every registered check.
 *
 * Runs as a daemon() thread (HIGH_PRIORITY, any core, not counted in
 * n_active), so it never delays shutdown. Shutdown only discards it while it
 * is parked in sleep() or runnable between sweeps; drivers' destroy paths
 * must therefore leave their check's state valid until every core has
 * entered shutdown.
 */
static void watchdog_daemon(void* unused){
  (void)unused;
  while (true){
    sleep(WATCHDOG_POLL_JIFFIES);
    unsigned now = (unsigned)__atomic_load_n((int*)&current_jiffies);
    int count = __atomic_load_n(&watchdog_check_count);
    for (int i = 0; i < count; i++){
      watchdog_checks[i](now);
    }
  }
}

// Append one check; the first registration creates the daemon.
void watchdog_register(void (*check)(unsigned now)){
  assert(check != NULL, "watchdog register: check function is NULL.\n");

  int count = __atomic_load_n(&watchdog_check_count);
  if (count >= WATCHDOG_MAX_CHECKS){
    int args[1] = {WATCHDOG_MAX_CHECKS};
    say("| watchdog register rejected: all %d check slots are in use\n", args);
    panic("watchdog register: raise WATCHDOG_MAX_CHECKS for the new driver.\n");
  }

  watchdog_checks[count] = check;
  __atomic_store_n(&watchdog_check_count, count + 1);

  if (count == 0){
    // The daemon is boot-lifetime and is never freed, so leak() its Fun.
    struct Fun* daemon_fun = leak(sizeof(struct Fun));
    daemon_fun->func = watchdog_daemon;
    daemon_fun->arg = NULL;
    daemon(daemon_fun, HIGH_PRIORITY, ANY_CORE);
  }
}
