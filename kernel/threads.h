#ifndef THREADS_H
#define THREADS_H

#include "TCB.h"
#include "queue.h"

#define TCB_STACK_SIZE 16384 // 16KiB

#define IDLE_STACK_SIZE 16384 // 16KiB
#define IDLE_STACKS_TOP 0x10000

extern struct SpinQueue global_ready_queue[PRIORITY_LEVELS][MLFQ_LEVELS];
extern struct SpinQueue reaper_queue;

extern int n_active;
extern int n_active_others; // number of running threads not counted in n_active

extern unsigned DEFAULT_INTERRUPT_MASK;

// true until the first thread is created, 
// after which we consider the system to be done with bootstrapping and fully operational
extern bool bootstrapping;

// initialize thread structures; should only be called once on one core
void threads_init(void);

// Switch away from the current thread and run func(arg) in the next context.
// `was` is restored when this thread resumes. The callback runs with interrupts
// enabled only when run_with_interrupts requests it; otherwise they are enabled
// after it returns. Interrupts must be disabled on entry, the current thread must
// be core->current_thread, and the callback must not modify the next TCB.
void block(unsigned was, void (*func)(void *), void *arg, bool run_with_interrupts);

// Perform a context switch from the current thread (me) to the next thread (next)
// Run func(arg) in the context of the next thread before switching to it
// Needs pointer to current_thread entry of the core's PerCore struct
// Assumes interrupts are disabled when this is called, and will re-enable them in the new thread's context
// run_with_interrupts determines whether to re-enable interrupts before calling the callback function, or after
// Assumes callback doesn't modify the 'next' TCB 
extern void context_switch(struct TCB* me, struct TCB* next, void (*func)(void *), void *arg, 
  struct TCB** cur_thread, int was, bool run_with_interrupts);

// called when a new thread first runs
// calls the thread's main function and calls stop() when it returns
void thread_entry(void);

// Attempt to deliver a synchronous signal to the current user thread.
//
// Returns false when the signal has no registered handler or a handler is
// already active. Returns true only after the handler calls sigreturn().
// A handler that exits, faults, or returns normally terminates the thread, so
// this function does not return in those cases.
bool try_run_current_signal_handler(int signal, unsigned arg1, unsigned arg2);

// Complete sigreturn for the current handler. This atomically orders handler
// completion with cross-core signal sends: a nonmaskable signal published
// before completion terminates the thread, while later sends remain pending
// for a subsequent final user-return boundary.
void finish_current_signal_handler(void);

// Process at most one asynchronous signal immediately before the current
// kernel activation performs its final rfe into user mode.
//
// This is deliberately not a scheduler operation. A runnable thread may have
// been awakened in the middle of a blocking syscall or page fault, before its
// C continuation has accepted a synchronization handoff and released outer
// resources. The trap/interrupt/exception wrapper calls this only after that
// continuation returns and before restoring the saved user frame.
void process_pending_signals_before_user_return(void);

// idle thread loop
// calls to block() context switch to here, 
// where we decide which thread to run next and switch to it
void event_loop(void);

// create a thread to run the given function, and add it to the global ready queue
void thread(struct Fun* thread_fun);

// same as thread(), but doesn't modify bootstrapping or n_active
// used to make stuff like reaper threads that won't count as active threads
// and leave the system in the bootstrapping phase
// leaks mem because it assumes these threads run forever
void setup_thread(struct Fun* thread_fun, enum ThreadPriority priority, enum CoreAffinity core_affinity);

/*
 * Keep the scheduler alive for asynchronous kernel work that outlives the
 * normal TCB which accepted it.
 *
 * begin preconditions:
 * - kernel mode in a normal, n_active-counted TCB
 * - called before the work is published to a daemon
 *
 * finish preconditions:
 * - kernel mode after the daemon has released every resource owned by exactly
 *   one previously acquired work reference
 *
 * The sequentially-consistent counter closes shutdown against accepted work:
 * event_loop() cannot leave while a reference exists. Each successful begin
 * must have exactly one finish. Exceeding the implementation's signed-count
 * safety limit is a kernel lifecycle error and panics without retaining the
 * rejected reference.
 */
void kernel_async_work_begin(void);
void kernel_async_work_finish(void);

// create a thread to run the given function, and add it to the global ready queue
// allows specifying the thread's priority and the core affinity
void thread_(struct Fun* thread_fun, enum ThreadPriority priority, enum CoreAffinity core_affinity);

// set up thread context for the first thread on this core (which is now the idle thread)
void bootstrap(void);

// voluntarily yield the CPU and re-queue the current thread
void yield(void);

// Block the current thread for at most INT_MAX ticks. Deadlines use modular
// 32-bit ordering, so this duration bound is required across jiffy wrap.
void sleep(unsigned jiffies);

// terminate the current thread and 
// place it on the reaper queue to eventually free its resources
// and set the return code that will be delivered to the parent via the child descriptor promise
void stop(unsigned rc);

// disable preemption and return whether it was previously enabled or not
bool preemption_disable(void);

// restore preemption to the given value
void preemption_restore(bool was);

// pin a thread to the current core, preventing it from being scheduled on other cores
enum CoreAffinity core_pin(void);

// allow a thread to be scheduled on any core
void core_unpin(enum CoreAffinity prev);

#endif // THREADS_H
