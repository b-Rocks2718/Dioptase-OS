/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef THREADS_H
#define THREADS_H

#include "TCB.h"
#include "queue.h"

extern struct SpinQueue global_ready_queue;
extern struct SpinQueue reaper_queue;

extern bool sd_wait_thread_0_pending; // is there about to be a thread waiting for SD drive 0?
extern struct TCB* sd_wait_thread_0; // thread waiting for SD drive 0

extern bool sd_wait_thread_1_pending; // is there about to be a thread waiting for SD drive 1?
extern struct TCB* sd_wait_thread_1; // thread waiting for SD drive 1

extern unsigned DEFAULT_INTERRUPT_MASK;

// true until the first thread is created, 
// after which we consider the system to be done with bootstrapping and fully operational
extern bool bootstrapping;

// initialize scheduler structures; should only be called once on one core
void threads_init(void);

// switch away from the current thread and run a completion callback
// Inputs: was is the interrupt mask to restore when this thread is resumed
// func/arg execute on the next context after the switch
// run_with_interrupts indicates whether to restore interrupts before running the callback
// If false, they are enabled after the callback returns
// Assumes callback doesn't modify the 'next' TCB
// Preconditions: interrupts are disabled; current thread is core->current_thread
void block(unsigned was, void (*func)(void *), void *arg, bool run_with_interrupts);

// called when a new thread first runs
// calls the thread's main function and calls stop() when it returns
void thread_entry(void);

// idle thread loop
// calls to block() context switch to here, 
// where we decide which thread to run next and switch to it
void event_loop(void);

// create a thread to run the given function, and add it to the global ready queue
void thread(struct Fun* thread_fun);

// set up thread context for the first thread on this core (which is now the idle thread)
void bootstrap(void);

// add a thread to the global ready queue, or if it's pinned, to its core's pinned queue
void global_queue_add(void* tcb);

// remove a thread from the global ready queue
struct TCB* global_queue_remove(void);

// add a thread to the core-local ready queue
void local_queue_add(void* tcb);

// remove a thread from the core-local ready queue
struct TCB* local_queue_remove(void);

// voluntarily yield the CPU and re-queue the current thread
void yield(void);

// block the current thread until a target jiffy count is reached
void sleep(unsigned jiffies);

// terminate the current thread and 
// place it on the reaper queue to eventually free its resources
void stop(void);

// disable preemption and return whether it was previously enabled or not
bool preemption_disable(void);

// restore preemption to the given value
void preemption_restore(bool was);

// pin a thread to the current core, preventing it from being scheduled on other cores
void core_pin(void);

// allow a thread to be scheduled on any core
void core_unpin(void);

#endif // THREADS_H
