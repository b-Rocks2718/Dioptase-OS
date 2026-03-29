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

#define GLOBAL_CHECK_INTERVAL 8 // every 8 iterations of the event loop, check the global queue for new work

// The ratios of these weights determine the relative frequency with which threads
// of different priorities are scheduled. For example, with the default weights
// of 4:2:1, high priority threads are scheduled approximately four times as
// often as low priority threads, and twice as often as mid priority threads.
#define HIGH_PRIORITY_WEIGHT 4 
#define MID_PRIORITY_WEIGHT 2
#define LOW_PRIORITY_WEIGHT 1

unsigned TIME_QUANTUM[MLFQ_LEVELS] = {2, 4, 8};
unsigned MLFQ_BOOST_INTERVAL = 500; // boost all threads to highest MLFQ level every 500 jiffies

unsigned REBALANCE_INTERVAL = 256; // only check for rebalancing every 256 iterations to avoid excessive overhead
#define MAX_REBALANCE_PERCENT 130 // rebalance if we have >130% of our ideal number of threads
#define MIN_REBALANCE_PERCENT 70 // rebalance if we have <70% of our ideal number of threads

// initialize scheduler structures; should only be called by threads_init
void scheduler_init(void){
  spin_queue_init(&global_ready_queue);
  spin_queue_init(&reaper_queue);

  for (int i = 0; i < MAX_CORES; i++) {
    for (int j = 0; j < PRIORITY_LEVELS; j++) {
      for (int k = 0; k < MLFQ_LEVELS; k++) {
        queue_init(&per_core_data[i].ready_queue[j][k]);
      }
    }
    sleep_queue_init(&per_core_data[i].sleep_queue);
    spin_queue_init(&per_core_data[i].pinned_queue);
    per_core_data[i].scheduler_iters = 0;
    per_core_data[i].mlfq_boost_pending = false;
    per_core_data[i].rebalance_pending = false;
  }

  sd_wait_thread_0_pending = false;
  sd_wait_thread_0 = NULL;

  sd_wait_thread_1_pending = false;
  sd_wait_thread_1 = NULL;
}

// add a thread to the global ready queue, or if it's pinned, to its core's pinned queue
void global_queue_add(void* tcb){
  struct TCB* thread = (struct TCB*)tcb;

  if (thread->core_affinity == ANY_CORE) {
    spin_queue_add(&global_ready_queue, thread);
    return;
  }

  // If the thread is pinned to a specific core, add it to that core's pinned queue
  // The idle thread on that core will move it to the ready queue
  struct PerCore* target_core = &per_core_data[thread->core_affinity];
  spin_queue_add(&target_core->pinned_queue, thread);
}

struct TCB* global_queue_remove(void){
  return spin_queue_remove(&global_ready_queue);
}

// add a thread to the core-local ready queue
void local_queue_add(void* tcb){
  struct TCB* thread = (struct TCB*)tcb;
  int was = interrupts_disable();
  // leave interrupts disabled around queue_add to avoid re-entrancy issues
  queue_add(&get_per_core()->ready_queue[thread->priority][thread->mlfq_level], thread);
  interrupts_restore(was);
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
  struct TCB* tcb = NULL;

  if (slot < HIGH_PRIORITY_WEIGHT) {
    tcb = local_remove_in_priority_order(per_core,
      HIGH_PRIORITY, NORMAL_PRIORITY, LOW_PRIORITY);
  } else if (slot < HIGH_PRIORITY_WEIGHT + MID_PRIORITY_WEIGHT) {
    tcb = local_remove_in_priority_order(per_core,
      NORMAL_PRIORITY, HIGH_PRIORITY, LOW_PRIORITY);
  } else {
    tcb = local_remove_in_priority_order(per_core,
      LOW_PRIORITY, NORMAL_PRIORITY, HIGH_PRIORITY);
  }

  interrupts_restore(was);
  return tcb;
}

// Remove a thread from either the local or global queue, preferring local work
// but periodically checking the global queue to prevent starvation
struct TCB* local_or_global_queue_remove(void){
  struct PerCore* core = get_per_core();
  struct TCB* next = NULL;

  // Periodically check global work first so threads woken onto the global
  // queue cannot starve behind a permanently "balanced" local queue set.
  if ((core->scheduler_iters % GLOBAL_CHECK_INTERVAL) == 0) {
    next = global_queue_remove();
  }

  if (next == NULL) {
    // if no global work (or we didn't check), run local work
    next = local_queue_remove();
  }

  if (next == NULL) {
    // if no local work, steal from global queue
    next = global_queue_remove();
  }

  return next;
}

// Boost all threads to the highest MLFQ level
// Only to be called by schedule_next_thread()
void mlfq_boost(void){
  struct PerCore* core = get_per_core();
  for (int i = LOW_PRIORITY; i <= HIGH_PRIORITY; i++) {
    for (int k = LEVEL_ONE; k <= LEVEL_TWO; k++) {
      struct TCB* tcb = queue_remove(&core->ready_queue[i][k]);
      while (tcb != NULL) {
        tcb->mlfq_level = LEVEL_ZERO;
        tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];
        local_queue_add(tcb);
        tcb = queue_remove(&core->ready_queue[i][k]);
      }
    }
  }
}

// Attempt to rebalance the number of threads between the local and global queues
// Only to be called by schedule_next_thread()
void rebalance_queues(void){
  struct PerCore* core = get_per_core();

  // Use ceiling division so remainder runnable threads do not remain stuck
  // on the global queue when the active count is not divisible by core count.
  unsigned total_active = __atomic_load_n(&n_active) + __atomic_load_n(&n_active_others);
  unsigned ideal = (total_active + CONFIG.num_cores - 1) / CONFIG.num_cores;

  unsigned local_size = 0;
  for (int i = LOW_PRIORITY; i <= HIGH_PRIORITY; i++) {
    for (int k = 0; k < MLFQ_LEVELS; k++) {
      local_size += __atomic_load_n(&core->ready_queue[i][k].size);
    }
  }

  unsigned global_size = __atomic_load_n(&global_ready_queue.size);

  if (local_size * 100 > ideal * MAX_REBALANCE_PERCENT) {
    // if we too many more than our ideal number of threads, move some to the global queue
    int to_move = local_size - ideal;
    unsigned scan_budget = local_size;
    int moved = 0;
    while (moved < to_move && scan_budget > 0) {
      struct TCB* tcb = local_queue_remove();
      if (tcb == NULL) break;
      if (tcb->core_affinity != ANY_CORE) {
        // Keep pinned threads on their home core's local queue
        local_queue_add(tcb);
      } else {
        global_queue_add(tcb);
        moved++;
      }
      scan_budget--;
    }
  } else if (local_size * 100 < ideal * MIN_REBALANCE_PERCENT) {
    // if we have too few threads compared to our ideal number of threads, try to take some from the global queue
    int to_move = ideal - local_size;
    for (int i = 0; i < to_move; i++) {
      struct TCB* tcb = global_queue_remove();
      if (tcb == NULL) break;
      local_queue_add(tcb);
    }
  }
}

// Choose the next thread to run on this core, or return NULL to stay idle
// We are running in the idle thread, so we cannot do any operation that may block
// (this implies no heap allocations)
struct TCB* schedule_next_thread(void){
  struct PerCore* core = get_per_core();

  // check sleep queue for threads that need to be woken up
  struct TCB* wakeup = sleep_queue_remove(&core->sleep_queue);
  while (wakeup != NULL) {
    local_queue_add(wakeup);
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
