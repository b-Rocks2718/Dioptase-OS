/*
 * Atomic primitive test.
 *
 * Validates:
 * - spin_lock_init clears the lock state
 * - spin_lock_try_acquire() and spin_lock_acquire() disable interrupts and
 *   spin_lock_release() restores the prior IMR value
 * - preempt_spin_lock_try_acquire() and preempt_spin_lock_acquire() disable
 *   preemption without changing the IMR and restore the caller's prior
 *   preemption state on release
 * - contended lock users preserve mutual exclusion and shared counter updates
 * - contended preempt spin lock users preserve mutual exclusion and restore
 *   preemption after each critical section
 * - spin_barrier_sync() does not release any participant early
 *
 * How:
 * - check the lock state immediately after initialization
 * - acquire the lock through both APIs and compare the live IMR state before,
 *   during, and after each operation
 * - repeat the same checks for the preempt spin lock, including the case where
 *   the caller already had preemption disabled before acquiring it
 * - spawn lock workers that increment a shared counter while checking that only
 *   one thread enters the critical section at a time
 * - spawn preempt-lock workers that check both mutual exclusion and preemption
 *   state while they serialize on the lock
 * - spawn barrier workers that all call spin_barrier_sync() and verify nobody
 *   returns before every participant arrived
 */

#include "../kernel/atomic.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define LOCK_WORKERS 6
#define LOCK_ROUNDS 64
#define LOCK_PAUSE_ITERS 16
#define PREEMPT_LOCK_WORKERS 2
#define PREEMPT_LOCK_ROUNDS 16
#define BARRIER_THREADS 4
#define PREEMPT_WAIT_BUDGET 20000
#define IMR_GLOBAL_ENABLE 0x80000000

static struct SpinLock test_lock;
static struct PreemptSpinLock preempt_test_lock;

static int critical_inside = 0;
static int shared_counter = 0;
static int lock_workers_done = 0;

static int preempt_critical_inside = 0;
static int preempt_shared_counter = 0;
static int preempt_lock_workers_done = 0;
static int preempt_lock_steps = 0;

static int barrier_counter = 0;
static int barrier_arrived = 0;
static int barrier_passed = 0;

// Report an integer mismatch with a consistent atomic-test prefix.
static void fail_int(char* msg, int got, int expected) {
  int args[2] = { got, expected };
  say("***atomic FAIL got=%d expected=%d\n", args);
  panic(msg);
}

// Report a hex mismatch for IMR and lock-state checks.
static void fail_hex(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***atomic FAIL got=0x%X expected=0x%X\n", args);
  panic(msg);
}

// Sample whether the current thread is preemptible without changing the final state.
static bool preemption_is_enabled(void) {
  bool was = preemption_disable();
  preemption_restore(was);
  return was;
}

// Hammer the shared spin lock and fail if two workers overlap inside it.
static void lock_worker(void* arg) {
  (void)arg;

  for (int i = 0; i < LOCK_ROUNDS; i++) {
    spin_lock_acquire(&test_lock);

    int prior_inside = __atomic_fetch_add(&critical_inside, 1);
    if (prior_inside != 0) {
      fail_int("atomic test: concurrent spin lock entry detected\n", prior_inside, 0);
    }

    __atomic_fetch_add(&shared_counter, 1);

    for (int spin = 0; spin < LOCK_PAUSE_ITERS; spin++) {
      pause();
    }

    int prior_leave = __atomic_fetch_add(&critical_inside, -1);
    if (prior_leave != 1) {
      fail_int("atomic test: spin lock exit count corrupted\n", prior_leave, 1);
    }

    spin_lock_release(&test_lock);
    yield();
  }

  __atomic_fetch_add(&lock_workers_done, 1);
}

// Hammer the shared preempt spin lock and verify it disables only preemption.
static void preempt_lock_worker(void* arg) {
  (void)arg;

  for (int i = 0; i < PREEMPT_LOCK_ROUNDS; i++) {
    preempt_spin_lock_acquire(&preempt_test_lock);

    if (preemption_is_enabled()) {
      fail_int("atomic test: preempt spin lock left preemption enabled\n", 1, 0);
    }
    if ((get_imr() & IMR_GLOBAL_ENABLE) == 0) {
      fail_hex("atomic test: preempt spin lock unexpectedly disabled interrupts\n",
               get_imr(), IMR_GLOBAL_ENABLE);
    }

    int prior_inside = __atomic_fetch_add(&preempt_critical_inside, 1);
    if (prior_inside != 0) {
      fail_int("atomic test: concurrent preempt spin lock entry detected\n", prior_inside, 0);
    }

    __atomic_fetch_add(&preempt_lock_steps, 1);
    __atomic_fetch_add(&preempt_shared_counter, 1);

    for (int spin = 0; spin < LOCK_PAUSE_ITERS; spin++) {
      pause();
    }

    int prior_leave = __atomic_fetch_add(&preempt_critical_inside, -1);
    if (prior_leave != 1) {
      fail_int("atomic test: preempt spin lock exit count corrupted\n", prior_leave, 1);
    }

    preempt_spin_lock_release(&preempt_test_lock);
    if (!preemption_is_enabled()) {
      fail_int("atomic test: preempt spin lock release did not re-enable preemption\n", 0, 1);
    }

    yield();
  }

  __atomic_fetch_add(&preempt_lock_workers_done, 1);
}

// Wait on the spin barrier and prove no thread escaped before the full group arrived.
static void barrier_worker(void* arg) {
  (void)arg;

  __atomic_fetch_add(&barrier_arrived, 1);
  spin_barrier_sync(&barrier_counter);

  int arrived = __atomic_load_n(&barrier_arrived);
  if (arrived != BARRIER_THREADS) {
    fail_int("atomic test: spin barrier released early\n", arrived, BARRIER_THREADS);
  }

  __atomic_fetch_add(&barrier_passed, 1);
}

// Allocate and start one worker for either the lock or barrier phase.
static void spawn_worker(void (*func)(void*)) {
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "atomic test: Fun allocation failed.\n");
  fun->func = func;
  fun->arg = NULL;
  thread(fun);
}

// Validate the single-threaded preempt spin lock state and restore contract.
static void check_preempt_lock_state(unsigned imr_before) {
  preempt_spin_lock_init(&preempt_test_lock);
  if (preempt_test_lock.the_lock != 0) {
    fail_int("atomic test: preempt_spin_lock_init did not clear the lock\n",
             preempt_test_lock.the_lock, 0);
  }
  if (preempt_test_lock.preempt_state != false) {
    fail_int("atomic test: preempt_spin_lock_init did not clear preempt_state\n",
             preempt_test_lock.preempt_state, false);
  }
  if (!preemption_is_enabled()) {
    fail_int("atomic test: preemption should start enabled before preempt lock checks\n", 0, 1);
  }

  if (!preempt_spin_lock_try_acquire(&preempt_test_lock)) {
    panic("atomic test: preempt_spin_lock_try_acquire failed on an unlocked lock\n");
  }
  if (preemption_is_enabled()) {
    fail_int("atomic test: preempt try_acquire left preemption enabled\n", 1, 0);
  }
  if (get_imr() != imr_before) {
    fail_hex("atomic test: preempt try_acquire changed the IMR\n", get_imr(), imr_before);
  }

  if (preempt_spin_lock_try_acquire(&preempt_test_lock)) {
    panic("atomic test: nested preempt_spin_lock_try_acquire unexpectedly succeeded\n");
  }
  if (preemption_is_enabled()) {
    fail_int("atomic test: failed preempt try_acquire restored preemption too early\n", 1, 0);
  }
  if (get_imr() != imr_before) {
    fail_hex("atomic test: failed preempt try_acquire changed the IMR\n", get_imr(), imr_before);
  }

  preempt_spin_lock_release(&preempt_test_lock);
  if (!preemption_is_enabled()) {
    fail_int("atomic test: preempt release did not restore enabled preemption\n", 0, 1);
  }
  if (get_imr() != imr_before) {
    fail_hex("atomic test: preempt release changed the IMR after try_acquire\n",
             get_imr(), imr_before);
  }

  preempt_spin_lock_acquire(&preempt_test_lock);
  if (preemption_is_enabled()) {
    fail_int("atomic test: preempt acquire left preemption enabled\n", 1, 0);
  }
  if (get_imr() != imr_before) {
    fail_hex("atomic test: preempt acquire changed the IMR\n", get_imr(), imr_before);
  }
  preempt_spin_lock_release(&preempt_test_lock);

  if (!preemption_is_enabled()) {
    fail_int("atomic test: preempt release did not restore enabled preemption after acquire\n",
             0, 1);
  }
  if (get_imr() != imr_before) {
    fail_hex("atomic test: preempt release changed the IMR after acquire\n", get_imr(), imr_before);
  }

  // If the caller had preemption disabled already, release must preserve that state.
  bool prev_preempt = preemption_disable();
  if (prev_preempt != true) {
    fail_int("atomic test: baseline preemption_disable should report enabled preemption\n",
             prev_preempt, true);
  }

  preempt_spin_lock_acquire(&preempt_test_lock);
  if (preemption_is_enabled()) {
    fail_int("atomic test: preempt lock should keep caller-disabled preemption off\n", 1, 0);
  }
  preempt_spin_lock_release(&preempt_test_lock);
  if (preemption_is_enabled()) {
    fail_int("atomic test: preempt release enabled preemption that the caller had disabled\n",
             1, 0);
  }

  preemption_restore(prev_preempt);
  if (!preemption_is_enabled()) {
    fail_int("atomic test: preemption_restore did not re-enable preemption after caller-disabled check\n",
             0, 1);
  }
}

void kernel_main(void) {
  say("***atomic test start\n", NULL);

  // First validate the single-threaded lock state and interrupt masking contract.
  spin_lock_init(&test_lock);
  if (test_lock.the_lock != 0) {
    fail_int("atomic test: spin_lock_init did not clear the lock\n", test_lock.the_lock, 0);
  }
  if (test_lock.interrupt_state != 0) {
    fail_int("atomic test: spin_lock_init did not clear interrupt_state\n",
             test_lock.interrupt_state, 0);
  }

  unsigned imr_before = get_imr();
  if ((imr_before & IMR_GLOBAL_ENABLE) == 0) {
    fail_hex("atomic test: kernel_main started with interrupts disabled\n", imr_before,
             IMR_GLOBAL_ENABLE);
  }

  if (!spin_lock_try_acquire(&test_lock)) {
    panic("atomic test: spin_lock_try_acquire failed on an unlocked lock\n");
  }

  unsigned imr_during_try = get_imr();
  if ((imr_during_try & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: try_acquire left interrupts enabled\n", imr_during_try, 0);
  }

  if (spin_lock_try_acquire(&test_lock)) {
    panic("atomic test: nested spin_lock_try_acquire unexpectedly succeeded\n");
  }

  unsigned imr_after_failed_try = get_imr();
  if ((imr_after_failed_try & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: failed try_acquire restored interrupts too early\n",
             imr_after_failed_try, 0);
  }

  spin_lock_release(&test_lock);
  if (get_imr() != imr_before) {
    fail_hex("atomic test: release did not restore IMR after try_acquire\n", get_imr(), imr_before);
  }

  spin_lock_acquire(&test_lock);
  unsigned imr_during_acquire = get_imr();
  if ((imr_during_acquire & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: acquire left interrupts enabled\n", imr_during_acquire, 0);
  }
  spin_lock_release(&test_lock);

  if (get_imr() != imr_before) {
    fail_hex("atomic test: release did not restore IMR after acquire\n", get_imr(), imr_before);
  }

  say("***atomic lock state ok\n", NULL);

  // Repeat the state checks for the preempt spin lock, which must preserve IMR.
  check_preempt_lock_state(imr_before);
  say("***atomic preempt lock state ok\n", NULL);

  // Then add contention and verify the lock still serializes every worker.
  critical_inside = 0;
  shared_counter = 0;
  lock_workers_done = 0;
  for (int i = 0; i < LOCK_WORKERS; i++) {
    spawn_worker(lock_worker);
  }

  while (__atomic_load_n(&lock_workers_done) != LOCK_WORKERS) {
    yield();
  }

  int expected_total = LOCK_WORKERS * LOCK_ROUNDS;
  if (__atomic_load_n(&shared_counter) != expected_total) {
    fail_int("atomic test: spin lock lost counter updates\n", __atomic_load_n(&shared_counter),
             expected_total);
  }
  if (__atomic_load_n(&critical_inside) != 0) {
    fail_int("atomic test: critical section occupancy did not return to zero\n",
             __atomic_load_n(&critical_inside), 0);
  }

  say("***atomic contention ok\n", NULL);

  // Add contention on the preempt spin lock and verify it restores preemption.
  preempt_critical_inside = 0;
  preempt_shared_counter = 0;
  preempt_lock_workers_done = 0;
  preempt_lock_steps = 0;
  for (int i = 0; i < PREEMPT_LOCK_WORKERS; i++) {
    spawn_worker(preempt_lock_worker);
  }

  for (int i = 0; i < PREEMPT_WAIT_BUDGET &&
                  __atomic_load_n(&preempt_lock_workers_done) != PREEMPT_LOCK_WORKERS;
       i++) {
    pause();
  }
  if (__atomic_load_n(&preempt_lock_workers_done) != PREEMPT_LOCK_WORKERS) {
    int args[4] = {
      __atomic_load_n(&preempt_lock_workers_done),
      PREEMPT_LOCK_WORKERS,
      __atomic_load_n(&preempt_shared_counter),
      __atomic_load_n(&preempt_lock_steps)
    };
    say("***atomic FAIL preempt_done=%d expected=%d shared=%d steps=%d\n", args);
    panic("atomic test: preempt spin lock workers did not finish\n");
  }

  int preempt_expected_total = PREEMPT_LOCK_WORKERS * PREEMPT_LOCK_ROUNDS;
  if (__atomic_load_n(&preempt_shared_counter) != preempt_expected_total) {
    fail_int("atomic test: preempt spin lock lost counter updates\n",
             __atomic_load_n(&preempt_shared_counter), preempt_expected_total);
  }
  if (__atomic_load_n(&preempt_critical_inside) != 0) {
    fail_int("atomic test: preempt critical section occupancy did not return to zero\n",
             __atomic_load_n(&preempt_critical_inside), 0);
  }
  if (!preemption_is_enabled()) {
    fail_int("atomic test: preemption stayed disabled after preempt lock contention\n", 0, 1);
  }

  say("***atomic preempt contention ok\n", NULL);

  // Finish with the spin barrier so every participant must wait for the full group.
  barrier_counter = BARRIER_THREADS;
  barrier_arrived = 0;
  barrier_passed = 0;
  for (int i = 0; i < BARRIER_THREADS; i++) {
    spawn_worker(barrier_worker);
  }

  while (__atomic_load_n(&barrier_passed) != BARRIER_THREADS) {
    yield();
  }

  if (__atomic_load_n(&barrier_arrived) != BARRIER_THREADS) {
    fail_int("atomic test: not all barrier participants arrived\n",
             __atomic_load_n(&barrier_arrived), BARRIER_THREADS);
  }
  if (__atomic_load_n(&barrier_counter) != 0) {
    fail_int("atomic test: barrier counter did not reach zero\n",
             __atomic_load_n(&barrier_counter), 0);
  }

  say("***atomic barrier ok\n", NULL);
  say("***atomic test complete\n", NULL);
}
