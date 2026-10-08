#ifndef WATCHDOG_H
#define WATCHDOG_H

/*
 * Kernel device watchdog: one persistent daemon that enforces software
 * deadlines for interrupt-driven device waits.
 *
 * The kernel has no timed wait, so sleep() is the only clock-driven wake path.
 * Drivers whose waits complete through device interrupts register one deadline
 * check here instead of each running a polling daemon. Every
 * WATCHDOG_POLL_JIFFIES the daemon calls each registered check with the
 * current jiffy count. A check runs in the daemon's kernel thread context
 * (interrupts enabled, preemptible), must not block for long, and typically
 * publishes a timeout result under the driver's own lock and then notifies
 * the waiting thread.
 *
 * The watchdog exists only for hardware that breaks its completion contract
 * (a wedged device or a lost interrupt line). Checks must not complete a
 * request early that its interrupt would complete anyway.
 */

// Interval between deadline sweeps. Deadline resolution is this coarse, which
// is fine for the multi-second deadlines it enforces.
#define WATCHDOG_POLL_JIFFIES 30

// Upper bound on registered checks; one per interrupt-driven driver.
#define WATCHDOG_MAX_CHECKS 4

/*
 * Register a deadline check and, on the first registration, start the
 * watchdog daemon.
 *
 * Preconditions: called only during single-threaded driver initialization in
 * kernel_entry (core 0, kernel mode) and never after boot. The check, and
 * all state it reads, must be initialized before this call because the daemon
 * may call it from the first sweep on. Registration of more than
 * WATCHDOG_MAX_CHECKS checks panics.
 */
void watchdog_register(void (*check)(unsigned now));

#endif // WATCHDOG_H
