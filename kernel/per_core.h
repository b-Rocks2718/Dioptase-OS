#ifndef PER_CORE_H
#define PER_CORE_H

#include "constants.h"
#include "TCB.h"
#include "queue.h"
#include "config.h"
#include "physmem.h"
#include "heap.h"

// Stores all core-local data
struct PerCore {
  // threads/scheduling
  struct TCB idle_thread;
  struct TCB* current_thread;
  struct Queue ready_queue[PRIORITY_LEVELS][MLFQ_LEVELS];
  struct Queue deferred_interrupt_wake_queue;
  struct SpinQueue pinned_queue;
  struct SleepQueue sleep_queue;
  unsigned scheduler_iters;
  // Core 0 publishes these from the PIT handler. The owning core reads them
  // with ordinary loads, so the cells are volatile.
  volatile bool mlfq_boost_pending;
  volatile bool rebalance_pending;

  struct CLHNode* idle_clh_node;
  
  // I/O: per-core SPSC rings filled by this core's device ISRs
  struct EventBuf key_events;
  struct EventBuf mouse_events;

  // allocator
  struct PhysmemLocalCache physmem_cache;

  struct FreeObject* free_lists[NUM_OBJECT_SIZES];
  unsigned free_list_sizes[NUM_OBJECT_SIZES];
};

extern struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct PerCore* get_per_core(void);

// Return the TCB currently running on this core. Callable from any kernel
// context: it masks interrupts internally so the result cannot belong to a
// different core after a migration between the core-ID read and the load.
struct TCB* get_current_tcb(void);

#endif // PER_CORE_H
