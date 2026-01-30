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

#include "atomic.h"
#include "machine.h"
#include "TCB.h"
#include "print.h"
#include "heap.h"
#include "threads.h"
#include "queue.h"
#include "per_core.h"
#include "debug.h"
#include "pit.h"
#include "interrupts.h"

struct SpinQueue ready_queue = {NULL, NULL, {0 }, 0};

int n_active = 0;
bool bootstrapping = true;

static void free_fun(struct Fun* fun) {
  if (fun->arg != NULL) {
    free(fun->arg);
  }
  free(fun);
}

static struct TCB* make_tcb(void){
  struct TCB* tcb = malloc(sizeof(struct TCB));

  tcb->flags = 0;

  tcb->r1 = 0;
  tcb->r2 = 0;
  tcb->r3 = 0;
  tcb->r4 = 0;
  tcb->r5 = 0;
  tcb->r6 = 0;
  tcb->r7 = 0;
  tcb->r8 = 0;
  tcb->r9 = 0;
  tcb->r10 = 0;
  tcb->r11 = 0;
  tcb->r12 = 0;
  tcb->r13 = 0;
  tcb->r14 = 0;
  tcb->r15 = 0;
  tcb->r16 = 0;
  tcb->r17 = 0;
  tcb->r18 = 0;
  tcb->r19 = 0;
  tcb->r20 = 0;
  tcb->r21 = 0;
  tcb->r22 = 0;
  tcb->r23 = 0;
  tcb->r24 = 0;
  tcb->r25 = 0;
  tcb->r26 = 0;
  tcb->r27 = 0;
  tcb->r28 = 0;

  // interrupts enabled, timer interrupt enabled
  tcb->imr = 0;

  tcb->next = NULL;

  return tcb;
}

static void free_tcb(void* tcb_ptr) {
  struct TCB* tcb = (struct TCB*)tcb_ptr;
  assert(tcb != NULL, "Error: trying to free resources of a NULL TCB.\n");
  assert(tcb->stack != NULL, "Error: TCB stack is already NULL.\n");
  free(tcb->stack);
  free_fun(tcb->thread_fun);
  free(tcb);

  __atomic_fetch_add(&n_active, -1);
}

void block(bool must, void (*func)(void *), void *arg) {
  unsigned was = disable_interrupts();
  struct PerCore* core = get_per_core();
  struct TCB* me = core->current_thread;
  struct TCB* idle = core->idle_thread;
  core->current_thread = idle; // prevents preemption
  restore_interrupts(was);

  context_switch(me, idle, func, arg, &core->current_thread);
}

void thread_entry(void) {
  struct TCB* current_tcb = get_current_tcb();
  struct Fun* thread_fun = current_tcb->thread_fun;
  if (thread_fun != NULL) {
    (*thread_fun->func)(thread_fun->arg);
  }

  // Thread cleanup happens after we switch to another context in stop().

  stop();
}

static void nothing(void* unused) {}

void event_loop(void) {
  /* only the idle thread can enter this function */
  while (__atomic_load_n(&bootstrapping) || (__atomic_load_n(&n_active) > 0)) {
    // if we actually are an idle thread, preemption is disabled
    // and we don't actually need to disable interrupts here
    int was = disable_interrupts();
    struct PerCore* core = get_per_core();
    assert(core->current_thread == core->idle_thread,
      "Error: only the idle thread can enter the event loop.\n");
    restore_interrupts(was);

    struct TCB* next = spin_queue_remove(&ready_queue);
    struct TCB* me = core->current_thread;
    if (next != NULL) {
      // We are about to run `next`; update per-core state before the switch so
      // thread_entry/stop see the correct current_thread.
      context_switch(me, next, nothing, NULL, &core->current_thread);
    } else {
      pause();
    }
  }
  if (get_core_id() == 0) {
    say("| finished in %d jiffies\n", (int*)&jiffies);

    say("| checking leaks\n", NULL);
    
    check_leaks();
    
    say("| Halting...\n", NULL);

    while (true) shutdown();
  } else {
    while (true) {
      pause();
    }
  }
}

void thread(struct Fun* thread_fun){
  struct TCB* tcb = make_tcb();
  __atomic_fetch_add(&n_active, 1);
  __atomic_store_n(&bootstrapping, false);

  unsigned* the_stack = malloc(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ret_addr = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->sp = (unsigned)(&the_stack[1023]);
  tcb->bp = (unsigned)(&the_stack[1023]);
  
  spin_queue_add(&ready_queue, tcb);
}

void bootstrap(void){
  struct TCB* tcb = leak(sizeof(struct TCB));

  tcb->flags = 0;

  tcb->r1 = 0;
  tcb->r2 = 0;
  tcb->r3 = 0;
  tcb->r4 = 0;
  tcb->r5 = 0;
  tcb->r6 = 0;
  tcb->r7 = 0;
  tcb->r8 = 0;
  tcb->r9 = 0;
  tcb->r10 = 0;
  tcb->r11 = 0;
  tcb->r12 = 0;
  tcb->r13 = 0;
  tcb->r14 = 0;
  tcb->r15 = 0;
  tcb->r16 = 0;
  tcb->r17 = 0;
  tcb->r18 = 0;
  tcb->r19 = 0;
  tcb->r20 = 0;
  tcb->r21 = 0;
  tcb->r22 = 0;
  tcb->r23 = 0;
  tcb->r24 = 0;
  tcb->r25 = 0;
  tcb->r26 = 0;
  tcb->r27 = 0;
  tcb->r28 = 0;
  
  tcb->next = NULL;

  // these values should never be used
  tcb->thread_fun = NULL;
  tcb->bp = 0;
  tcb->sp = 0;
  tcb->ret_addr = 0;
  tcb->psr = 1;
  tcb->imr = 0;

  tcb->stack = (unsigned*)(0x10000 - (get_core_id() * 0x4000));

  // interrupts should be disabled when calling this function
  int was = disable_interrupts();
  struct PerCore* core = get_per_core();
  assert((was & 0x80000000) == 0, "Error: interrupts should be disabled when bootstrapping thread context.\n");
  core->current_thread = tcb;
  core->idle_thread = tcb;
  restore_interrupts(was);
}

void add_tcb(void* tcb){
  spin_queue_add(&ready_queue, (struct TCB*)tcb);
}

void yield(void){
  struct TCB* tcb = get_current_tcb();
  block(false, add_tcb, (void*)tcb);
}

void stop(void) {
  unsigned was = disable_interrupts();
  struct PerCore* core = get_per_core();
  bool is_idle = (core->current_thread == core->idle_thread);
  restore_interrupts(was);
  if (is_idle) {
    // idle thread should not be stopped
    event_loop();
  } else {
    // free current thread resources and block forever
    assert(n_active > 0, "Error: no active threads to stop.\n");
    block(true, free_tcb, (void*)get_current_tcb());
  }

  panic("Error: unreachable code reached in stop().\n");
}
