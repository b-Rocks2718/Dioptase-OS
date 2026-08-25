#ifndef TCB_H
#define TCB_H

#include "constants.h"
#include "sys.h"

// function and argument for a thread to run
struct Fun {
  void (*func)(void *);
  void *arg;
};

// The core a thread is pinned to, if any
enum CoreAffinity {
  CORE_0 = 0,
  CORE_1 = 1,
  CORE_2 = 2,
  CORE_3 = 3,
  ANY_CORE = 0xFFFFFFFF
};

#define PRIORITY_LEVELS 3
#define MLFQ_LEVELS 3

// priority levels for threads, used for scheduling decisions
enum ThreadPriority {
  LOW_PRIORITY = 0,
  NORMAL_PRIORITY = 1,
  HIGH_PRIORITY = 2,
};

enum MLFQ_LEVEL {
  LEVEL_ZERO = 0,
  LEVEL_ONE = 1,
  LEVEL_TWO = 2
};

// forward declaration of VME struct to avoid circular dependency between TCB and VME
struct VME;

struct Node;

#define MAX_SIGNALS 32

// Thread Control Block
// One per thread, stores all info about the thread including its context for switching
struct TCB {
  // callee-saved registers
  unsigned r20; // offset 0
  unsigned r21; // offset 4
  unsigned r22; // offset 8
  unsigned r23; // offset 12
  unsigned r24; // offset 16
  unsigned r25; // offset 20
  unsigned r26; // offset 24
  unsigned r27; // offset 28
  unsigned r28; // offset 32

  // function state registers
  unsigned sp;  // offset 36
  unsigned bp;  // offset 40
  unsigned ra;  // offset 44
  
  // control registers
  unsigned flags;    // offset 48
  unsigned psr;      // offset 52
  unsigned imr;      // offset 56
  unsigned pid;      // offset 60
  unsigned fault_addr;  // offset 64
  unsigned fault_flags; // offset 68
  unsigned ksp; // offset 72

  // other thread state
  unsigned uaccess_active;   // offset 76
  unsigned uaccess_err_addr; // offset 80

  unsigned* stack;
  struct Fun* thread_fun;

  bool can_preempt;
  enum CoreAffinity core_affinity;
  enum ThreadPriority priority;
  enum MLFQ_LEVEL mlfq_level;
  int remaining_quantum;
  unsigned wakeup_jiffies;

  struct FileDescriptor* file_descriptors[MAX_FILE_DESCRIPTORS];
  struct SemDescriptor* sem_descriptors[MAX_SEM_DESCRIPTORS];
  struct ChildDescriptor* child_descriptors[MAX_CHILD_DESCRIPTORS];

  struct ChildDescriptor* parent_promise;
  
  struct Node* cwd;
  char *cwd_path;

  struct VME* vme_list;

  // Cross-core senders and the current thread's final user-return path
  // serialize this bitmap through parent_promise->state_lock while child_tcb
  // is live. The scheduler must not consume pending signals: a runnable TCB
  // may still have a suspended kernel continuation that owns resources.
  unsigned pending_signals;

  // A set bit defers the corresponding maskable signal; it does not discard a
  // pending instance. Only the running thread mutates this field, and the
  // final user-return path reads it while executing on behalf of that thread.
  unsigned signal_mask;

  // Only the running thread registers handlers. Final user-return paths and
  // synchronous exception paths read these entries while executing on behalf
  // of that same TCB, so the TCB cannot be active concurrently on another core.
  void* signal_handlers[MAX_SIGNALS];
  bool in_signal_handler;
  unsigned signal_stack_top;

  struct CLHNode* my_node; // used as a ticket for accessing any kind of spinlock
  struct CLHNode* my_pred;

  // setup_thread() TCBs are boot-lifetime daemons. Normal completion ignores
  // them when deciding to shut down, so scheduler teardown may detach only
  // TCBs carrying this explicit ownership marker from residual ready/sleep
  // queues. This field is after all assembly-addressed context members.
  bool is_daemon;

  struct TCB* next;
};

#endif // TCB_H
