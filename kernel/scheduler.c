#include "scheduler.h"
#include "threads.h"
#include "print.h"
#include "debug.h"
#include "queue.h"
#include "per_core.h"
#include "atomic.h"
#include "TCB.h"
#include "machine.h"
#include "interrupts.h"
#include "config.h"

/*
  Once I get user mode working, I'll spend some time tuning these parameters
*/

#define GLOBAL_CHECK_INTERVAL 2 // check shared runnable work on every other scheduling pass so one hot local queue cannot hide runnable peers on other cores
#define MIN_LOCAL_RUNNABLE_THREADS 1 // a core only needs one admitted peer before it can keep filling opportunistically from the shared bucketed queues
#define MAX_GLOBAL_ADMISSIONS_PER_PASS 1 // one admission per pass avoids one core vacuuming a whole weighted batch before other cores can participate

// The ratios of these weights determine the relative frequency with which threads
// of different priorities are scheduled. For example, with the default weights
// of 4:2:1, high priority threads are scheduled approximately four times as
// often as low priority threads, and twice as often as mid priority threads.
#define HIGH_PRIORITY_WEIGHT 4 
#define MID_PRIORITY_WEIGHT 2
#define LOW_PRIORITY_WEIGHT 1

unsigned TIME_QUANTUM[MLFQ_LEVELS] = {2, 4, 8};
unsigned MLFQ_BOOST_INTERVAL = 256; // boost queued work often enough to avoid indefinite starvation without constantly resetting CPU-bound threads

unsigned REBALANCE_INTERVAL = 512; // rebalance often enough that per-core priority/MLFQ skew does not persist for long on 4-core runs
#define MAX_REBALANCE_PERCENT 130 // rebalance if we have >130% of our ideal number of threads
#define MIN_REBALANCE_PERCENT 70 // rebalance if we have <70% of our ideal number of threads

static int global_admission_iters;

// Map one scheduler slot into the preferred priority order for that slot
static void scheduler_priority_order(unsigned slot,
                                     enum ThreadPriority* first,
                                     enum ThreadPriority* second,
                                     enum ThreadPriority* third) {
  if (slot < HIGH_PRIORITY_WEIGHT) {
    *first = HIGH_PRIORITY;
    *second = NORMAL_PRIORITY;
    *third = LOW_PRIORITY;
  } else if (slot < HIGH_PRIORITY_WEIGHT + MID_PRIORITY_WEIGHT) {
    *first = NORMAL_PRIORITY;
    *second = HIGH_PRIORITY;
    *third = LOW_PRIORITY;
  } else {
    *first = LOW_PRIORITY;
    *second = NORMAL_PRIORITY;
    *third = HIGH_PRIORITY;
  }
}

// initialize scheduler structures; should only be called by threads_init
void scheduler_init(void){
  spin_queue_init(&reaper_queue);

  // init global ready queues
  for (int priority = LOW_PRIORITY; priority <= HIGH_PRIORITY; priority++) {
    for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
      spin_queue_init(&global_ready_queue[priority][level]);
    }
  }

  // init per-core ready queues
  for (int i = 0; i < MAX_CORES; i++) {
    for (int j = 0; j < PRIORITY_LEVELS; j++) {
      for (int k = 0; k < MLFQ_LEVELS; k++) {
        queue_init(&per_core_data[i].ready_queue[j][k]);
      }
    }
    queue_init(&per_core_data[i].deferred_interrupt_wake_queue);
    sleep_queue_init(&per_core_data[i].sleep_queue);
    spin_queue_init(&per_core_data[i].pinned_queue);

    per_core_data[i].scheduler_iters = 0;
    per_core_data[i].mlfq_boost_pending = false;
    per_core_data[i].rebalance_pending = false;
  }

  global_admission_iters = 0;
}

// Add one ANY_CORE thread to the shared ready buckets that mirror the per-core
// local ready queues
static void global_bucket_add(struct TCB* thread) {
  assert(thread != NULL, "scheduler: global bucket add got NULL thread.\n");
  assert(thread->core_affinity == ANY_CORE,
    "scheduler: pinned thread cannot enter global ready buckets.\n");
  spin_queue_add(&global_ready_queue[thread->priority][thread->mlfq_level], thread);
}

// Remove one thread from an exact shared ready bucket.
static struct TCB* global_bucket_remove(enum ThreadPriority priority,
                                        enum MLFQ_LEVEL level) {
  return spin_queue_remove(&global_ready_queue[priority][level]);
}

// add a thread to the global ready queues, or if it's pinned, to its core's pinned queue
void global_queue_add(void* tcb){
  struct TCB* thread = (struct TCB*)tcb;

  if (thread->core_affinity == ANY_CORE) {
    global_bucket_add(thread);
    return;
  }

  // If the thread is pinned to a specific core, add it to that core's pinned queue
  // The idle thread on that core will move it to the ready queue
  struct PerCore* target_core = &per_core_data[thread->core_affinity];
  spin_queue_add(&target_core->pinned_queue, thread);
}

// Remove a thread from the global ready queues according to the admission policy
struct TCB* global_queue_remove(void){
  int old_slot = __atomic_fetch_add(&global_admission_iters, 1);
  unsigned slot = old_slot %
    (HIGH_PRIORITY_WEIGHT + MID_PRIORITY_WEIGHT + LOW_PRIORITY_WEIGHT);
  enum ThreadPriority first;
  enum ThreadPriority second;
  enum ThreadPriority third;
  scheduler_priority_order(slot, &first, &second, &third);

  // the order we check priorities within this admission slot is determined
  // by scheduler_priority_order(). We try the highest-weighted priority first,
  // then the second, then the third.

  // Within each priority, we check MLFQ levels from LEVEL_ZERO to LEVEL_TWO,
  // returning the first thread we find.
  struct TCB* tcb = NULL;
  for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    tcb = global_bucket_remove(first, level);
    if (tcb != NULL) return tcb;
  }

  for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    tcb = global_bucket_remove(second, level);
    if (tcb != NULL) return tcb;
  }

  for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
    tcb = global_bucket_remove(third, level);
    if (tcb != NULL) return tcb;
  }

  return NULL;
}

// add a thread to the core-local ready queue
void local_queue_add(void* tcb){
  struct TCB* thread = (struct TCB*)tcb;
  int was = interrupts_disable();
  // leave interrupts disabled around queue_add to avoid re-entrancy issues
  queue_add(&get_per_core()->ready_queue[thread->priority][thread->mlfq_level], thread);
  interrupts_restore(was);
}

// Count the runnable threads already admitted to this core's local ready queues.
// schedule_next_thread() uses this to decide when the local queue set is too small
// and should pull more work from the global admission queue.
static unsigned local_ready_queue_size(struct PerCore* core) {
  unsigned total = 0;
  for (int priority = LOW_PRIORITY; priority <= HIGH_PRIORITY; priority++) {
    for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
      total += __atomic_load_n(&core->ready_queue[priority][level].size);
    }
  }
  return total;
}

// Read the current size of one local ready bucket on the given core
static unsigned local_bucket_size(struct PerCore* core,
                                  enum ThreadPriority priority,
                                  enum MLFQ_LEVEL level) {
  return __atomic_load_n(&core->ready_queue[priority][level].size);
}

// Count the threads waiting in one shared global bucket
static unsigned global_bucket_size(enum ThreadPriority priority,
                                   enum MLFQ_LEVEL level) {
  return spin_queue_size(&global_ready_queue[priority][level]);
}

// Count all ready threads currently present in one (priority, level) bucket
// across every core-local ready queue plus the shared global ready queues
static unsigned total_ready_bucket_size(enum ThreadPriority priority,
                                        enum MLFQ_LEVEL level) {
  unsigned total = global_bucket_size(priority, level);

  for (int core_id = 0; core_id < CONFIG.num_cores; core_id++) {
    total += local_bucket_size(&per_core_data[core_id], priority, level);
  }

  return total;
}

// Move one globally queued thread into this core's runnable MLFQ queues
static bool admit_one_global_thread(void) {
  struct TCB* thread = global_queue_remove();
  if (thread == NULL) return false;

  assert(thread->core_affinity == ANY_CORE,
    "scheduler: pinned thread reached global ready queue.\n");
  local_queue_add(thread);
  return true;
}

// Pull a bounded amount of work from the global admission queue into this core's
// local MLFQ queues
static void admit_global_work(struct PerCore* core) {
  unsigned local_size = local_ready_queue_size(core);
  unsigned budget = 0;
  bool periodic_check = ((core->scheduler_iters % GLOBAL_CHECK_INTERVAL) == 0);

  if (local_size < MIN_LOCAL_RUNNABLE_THREADS) {
    budget = MIN_LOCAL_RUNNABLE_THREADS - local_size;
  } else if (periodic_check) {
    budget = 1;
  }

  if (budget > MAX_GLOBAL_ADMISSIONS_PER_PASS) {
    budget = MAX_GLOBAL_ADMISSIONS_PER_PASS;
  }

  while (budget > 0) {
    if (!admit_one_global_thread()) break;
    budget--;
  }
}

// Charge one unit of the current MLFQ level's budget to a voluntary yield
void scheduler_charge_yield(struct TCB* tcb) {
  assert(tcb != NULL, "scheduler_charge_yield: tcb is NULL.\n");

  if (tcb->remaining_quantum > 0) {
    tcb->remaining_quantum--;
  }

  if (tcb->remaining_quantum <= 0) {
    if (tcb->mlfq_level < LEVEL_TWO) {
      tcb->mlfq_level++;
    }
    tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];
  }
}

// Return one blocked thread to the runnable set while preserving its declared
// core affinity
void scheduler_wake_thread(struct TCB* tcb) {
  assert(tcb != NULL, "scheduler_wake_thread: tcb is NULL.\n");

  if (tcb->core_affinity == get_core_id()) {
    local_queue_add(tcb);
    return;
  }

  global_queue_add(tcb);
}

// Interrupt-safe wakeup path for device ISRs
// - ANY_CORE and same-core pinned threads are admitted directly to this core's
//   local ready queues in bounded time without taking a spin lock
// - remote pinned threads are appended to a current-core deferred queue and
//   will be handed to scheduler_wake_thread() by schedule_next_thread()
void scheduler_wake_thread_from_interrupt(struct TCB* tcb) {
  assert(tcb != NULL, "scheduler_wake_thread_from_interrupt: tcb is NULL.\n");

  queue_add(&get_per_core()->deferred_interrupt_wake_queue, tcb);
}

// MLFQ removes from the highest non-empty level first
struct TCB* mlfq_remove(struct Queue* mlfq) {
  for (int i = 0; i < MLFQ_LEVELS; i++) {
    struct TCB* tcb = queue_remove(&mlfq[i]);
    if (tcb != NULL) return tcb;
  }
  return NULL;
}

// Remove from the first non-empty queue in the given priority order.
// local_queue_remove() uses this to prefer one priority for the current slot
// while still falling back to lower- or higher-priority work if the preferred
// queue is empty. Must be called with interrupts disabled to avoid re-entrancy issues
struct TCB* local_remove_in_priority_order(struct PerCore* per_core,
  enum ThreadPriority first, enum ThreadPriority second, enum ThreadPriority third) {
  struct TCB* tcb = mlfq_remove(per_core->ready_queue[first]);
  if (tcb != NULL) return tcb;

  tcb = mlfq_remove(per_core->ready_queue[second]);
  if (tcb != NULL) return tcb;

  return mlfq_remove(per_core->ready_queue[third]);
}

// remove a thread from the core-local ready queue
struct TCB* local_queue_remove(void){
  int was = interrupts_disable();
  struct PerCore* per_core = get_per_core();
  unsigned slot = per_core->scheduler_iters % 
    (HIGH_PRIORITY_WEIGHT + MID_PRIORITY_WEIGHT + LOW_PRIORITY_WEIGHT);
  enum ThreadPriority first;
  enum ThreadPriority second;
  enum ThreadPriority third;
  scheduler_priority_order(slot, &first, &second, &third);
  struct TCB* tcb = NULL;
  tcb = local_remove_in_priority_order(per_core, first, second, third);

  interrupts_restore(was);
  return tcb;
}

// Remove a thread from the current core's local ready queues after first admitting
// any needed global work into those local queues. Caller must be the current core's idle thread
struct TCB* local_or_global_queue_remove(void){
  struct PerCore* core = get_per_core();
  admit_global_work(core);
  return local_queue_remove();
}

// Boost all threads to the highest MLFQ level
// Only to be called by schedule_next_thread()
void mlfq_boost(void){
  struct PerCore* core = get_per_core();
  for (int i = LOW_PRIORITY; i <= HIGH_PRIORITY; i++) {
    for (int k = LEVEL_ONE; k <= LEVEL_TWO; k++) {
      // empty higher-level queues and put contents in level 0

      // local queue boost
      struct TCB* tcb = queue_remove(&core->ready_queue[i][k]);
      while (tcb != NULL) {
        tcb->mlfq_level = LEVEL_ZERO;
        tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];
        local_queue_add(tcb);
        tcb = queue_remove(&core->ready_queue[i][k]);
      }

      // shared-global bucket boost
      if (get_core_id() == 0){
        // only core 0 boosts shared-global buckets
        tcb = global_bucket_remove(i, k);
        while (tcb != NULL) {
          tcb->mlfq_level = LEVEL_ZERO;
          tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];
          global_bucket_add(tcb);
          tcb = global_bucket_remove(i, k);
        }
      }
    }
  }
}

// Move up to to_move threads from one local bucket back to the matching
// shared-global bucket
static void move_local_bucket_to_global(struct PerCore* core,
                                        enum ThreadPriority priority,
                                        enum MLFQ_LEVEL level,
                                        unsigned to_move) {
  int was = interrupts_disable();
  unsigned scan_budget = local_bucket_size(core, priority, level);

  while (to_move > 0 && scan_budget > 0) {
    struct TCB* tcb = queue_remove(&core->ready_queue[priority][level]);
    if (tcb == NULL) break;

    if (tcb->core_affinity != ANY_CORE) {
      queue_add(&core->ready_queue[priority][level], tcb);
    } else {
      global_bucket_add(tcb);
      to_move--;
    }

    scan_budget--;
  }

  interrupts_restore(was);
}

// Move up to to_move threads from one exact shared-global bucket into the
// matching local ready bucket on this core
static void move_global_bucket_to_local(struct PerCore* core,
                                        enum ThreadPriority priority,
                                        enum MLFQ_LEVEL level,
                                        unsigned to_move) {
  while (to_move > 0) {
    struct TCB* tcb = global_bucket_remove(priority, level);
    if (tcb == NULL) break;

    int was = interrupts_disable();
    queue_add(&core->ready_queue[priority][level], tcb);
    interrupts_restore(was);
    to_move--;
  }
}

// Attempt to rebalance the number of threads between the local and global queues
// bucket-by-bucket, preserving both priority class and MLFQ level
// Only to be called by schedule_next_thread()
void rebalance_queues(void){
  struct PerCore* core = get_per_core();

  for (int priority = LOW_PRIORITY; priority <= HIGH_PRIORITY; priority++) {
    for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
      unsigned total_bucket = total_ready_bucket_size(priority, level);
      if (total_bucket == 0) continue;

      unsigned ideal = (total_bucket + CONFIG.num_cores - 1) / CONFIG.num_cores;
      unsigned local_bucket = local_bucket_size(core, priority, level);

      if (local_bucket * 100 > ideal * MAX_REBALANCE_PERCENT) {
        move_local_bucket_to_global(core, priority, level, local_bucket - ideal);
      } else if (local_bucket * 100 < ideal * MIN_REBALANCE_PERCENT) {
        move_global_bucket_to_local(core, priority, level, ideal - local_bucket);
      }
    }
  }
}

// Choose the next thread to run on this core, or return NULL to stay idle
// We are running in the idle thread, so we cannot do any operation that may block
// (this implies no heap allocations)
struct TCB* schedule_next_thread(void){
  struct PerCore* core = get_per_core();
  unsigned was = interrupts_disable();
  struct TCB* deferred = queue_remove_all(&core->deferred_interrupt_wake_queue);
  interrupts_restore(was);

  while (deferred != NULL) {
    struct TCB* next_deferred = deferred->next;
    deferred->next = NULL;
    scheduler_wake_thread(deferred);
    deferred = next_deferred;
  }

  // check sleep queue for threads that need to be woken up
  struct TCB* wakeup = sleep_queue_remove(&core->sleep_queue);
  while (wakeup != NULL) {
    scheduler_wake_thread(wakeup);
    wakeup = sleep_queue_remove(&core->sleep_queue);
  }

  // empty pinned queue into local ready queue
  struct TCB* pinned = spin_queue_remove_all(&core->pinned_queue);
  while (pinned != NULL) {
    struct TCB* next = pinned->next;
    pinned->next = NULL;
    local_queue_add(pinned);
    pinned = next;
  }

  if (core->rebalance_pending) {
    rebalance_queues();
    __atomic_store_n(&core->rebalance_pending, false);
  }

  if (core->mlfq_boost_pending) {
    mlfq_boost();
    __atomic_store_n(&core->mlfq_boost_pending, false);
  }

  struct TCB* next = local_or_global_queue_remove();

  core->scheduler_iters++;

  return next;
}

void set_priority(enum ThreadPriority priority){
  int was = interrupts_disable();
  struct TCB* current = get_current_tcb();
  if (current != NULL) {
    current->priority = priority;
  }
  interrupts_restore(was);
}
