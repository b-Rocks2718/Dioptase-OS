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
#include "config.h"
#include "ps2.h"

struct SpinQueue ready_queue;
struct SpinQueue reaper_queue;

struct SleepQueue sleep_queue;

int n_active = 0;
bool bootstrapping = true;

struct TCB idle_tcbs[MAX_CORES];

static void free_fun(struct Fun* fun) {
  if (fun->arg != NULL) {
    free(fun->arg);
  }
  free(fun);
}

static void free_tcb(struct TCB* tcb) {
  assert(tcb != NULL, "trying to free resources of a NULL TCB.\n");
  assert(tcb->stack != NULL, "TCB stack is already NULL.\n");
  free(tcb->stack);
  free_fun(tcb->thread_fun);
  free(tcb);

  __atomic_fetch_add(&n_active, -1);
}

static void reaper(void){
  while (true){
    struct TCB* tcb = spin_queue_remove_all(&reaper_queue);
    while (tcb != NULL){
      struct TCB* prev = tcb;
      tcb = tcb->next;
      free_tcb(prev);
    }
    yield();
  }
  panic("reaper thread tried to exit\n");
}

static struct TCB* make_tcb(bool leak_mem){
  struct TCB* tcb = leak_mem ? leak(sizeof(struct TCB)) : malloc(sizeof(struct TCB));

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
  tcb->imr = 0;//0x80000001;

  tcb->can_preempt = true;

  tcb->next = NULL;

  return tcb;
}

// same as thread(), but doesn't modify bootstrapping or n_active
// used to make stuff like reaper threads that won't count as active threads
// and leave the system in the bootstrapping phase
// leaks mem because it assumes these threads run forever
static void setup_thread(struct Fun* thread_fun){
  struct TCB* tcb = make_tcb(true);

  unsigned* the_stack = leak(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ret_addr = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->sp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  
  spin_queue_add(&ready_queue, tcb);
}

void threads_init(void){
  spin_queue_init(&ready_queue);
  spin_queue_init(&reaper_queue);
  sleep_queue_init(&sleep_queue);

  struct Fun* reaper_fun = leak(sizeof (struct Fun));
  reaper_fun->func = (void (*)(void *))reaper;
  reaper_fun->arg = NULL;

  setup_thread(reaper_fun);
}

// Purpose: switch away from the current thread and run a completion callback.
// Inputs: was is the interrupt mask to restore when this thread is resumed;
// func/arg execute on the next context after the switch.
// Preconditions: interrupts are disabled; current thread is core->current_thread.
// Postconditions: current thread state saved; next context runs func(arg).
// Invariants: core->current_thread refers to the running thread on entry.
// CPU state assumptions: kernel mode; local interrupts disabled.
void block(unsigned was, void (*func)(void *), void *arg) {
  struct PerCore* core = get_per_core();
  struct TCB* me = core->current_thread;
  struct TCB* idle = core->idle_thread;

  context_switch(me, idle, func, arg, &core->current_thread, was);
}

void thread_entry(void) {
  int was = disable_interrupts();
  struct TCB* current_tcb = get_current_tcb();
  restore_interrupts(was);
  struct Fun* thread_fun = current_tcb->thread_fun;
  if (thread_fun != NULL) {
    // Catch corrupted thread trampoline state before an indirect branch can jump to 0x0.
    if (thread_fun->func == NULL) {
      int args[4] = {
        get_core_id(),
        (int)current_tcb,
        (int)thread_fun,
        (int)thread_fun->arg
      };
      say("| thread_entry null func core=%d tcb=0x%X fun=0x%X arg=0x%X\n", args);
      panic("thread_entry: thread_fun->func is NULL.\n");
    }
    (*thread_fun->func)(thread_fun->arg);
  }

  // Thread cleanup:
  // stop() places thread in reaper queue
  // reaper thread eventually frees thread

  stop();
}

static void nothing(void* unused) {}

void event_loop(void) {
  /* only the idle thread can enter this function */
  while (__atomic_load_n(&bootstrapping) || (__atomic_load_n(&n_active) > 0)) {
    
    // check sleep queue
    struct TCB* wakeup = sleep_queue_remove(&sleep_queue);
    while (wakeup != NULL) {
      spin_queue_add(&ready_queue, wakeup);
      wakeup = sleep_queue_remove(&sleep_queue);
    }

    struct TCB* next = spin_queue_remove(&ready_queue);

    // if we actually are an idle thread, preemption is disabled
    // and we don't actually need to disable interrupts here
    // this is just for debugging
    int was = disable_interrupts();
    struct PerCore* core = get_per_core();
    assert(core != NULL, "per-core data is NULL.\n");
    if (core->current_thread != core->idle_thread) {
      int args[2] = {get_core_id(), (int)core->current_thread};
      say("core %d current thread: 0x%X\n", args);
      panic("only idle thread can enter event loop.\n");
    }

    struct TCB* me = core->current_thread;

    if (next != NULL) {
      context_switch(me, next, nothing, NULL, &core->current_thread, was);
    } else {
      restore_interrupts(was);
      pause();
    }
  }
  if (get_core_id() == 0) {
    say("| finished in %d jiffies\n", (int*)&current_jiffies);

    say("| checking leaks\n", NULL);
    
    check_leaks();

    if (CONFIG.use_vga){
      say("| Press Q to exit...\n", NULL);

      // Discard any stale keyboard events so only a fresh user keypress exits.
      while (getkey() != 0);

      // Wait for a full 'q' key cycle (make then break).
      // This avoids accidental exit from a stray/glitched make event.
      int saw_q_make = 0;
      while (true) {
        int key = getkey();
        if (key == 0) continue;

        int is_release = ((key & 0xFF00) != 0);
        key &= 0xFF;

        if (!is_release) {
          saw_q_make = (key == 'q' || key == 'Q');
          continue;
        }

        if (saw_q_make && (key == 'q' || key == 'Q')) break;
        saw_q_make = 0;
      }
    } else {
      say("| Halting...\n", NULL);
    }

    while (true) shutdown();
  } else {
    while (true) pause();
  }
}

void thread(struct Fun* thread_fun){
  struct TCB* tcb = make_tcb(false);
  __atomic_fetch_add(&n_active, 1);
  __atomic_store_n(&bootstrapping, false);

  unsigned* the_stack = malloc(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ret_addr = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->sp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  
  spin_queue_add(&ready_queue, tcb);
}

void bootstrap(void){
  // interrupts should be disabled when calling this function
  // but we'll be safe
  int was = disable_interrupts();
  int me = get_core_id();
  struct TCB* tcb = &idle_tcbs[me];

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
  tcb->can_preempt = false;

  // these values should never be used
  tcb->thread_fun = NULL;
  tcb->bp = 0;
  tcb->sp = 0;
  tcb->ret_addr = 0;
  tcb->psr = 1;
  tcb->imr = 0;

  tcb->stack = (unsigned*)(0x10000 - (me * 0x4000));

  struct PerCore* core = get_per_core();
  assert((was & 0x80000000) == 0, "interrupts should be disabled when bootstrapping thread context.\n");
  core->current_thread = tcb;
  core->idle_thread = tcb;
  restore_interrupts(was);
}

void add_tcb(void* tcb){
  spin_queue_add(&ready_queue, (struct TCB*)tcb);
}

// voluntarily yield the CPU and re-queue the current thread.
void yield(void){
  unsigned was = disable_interrupts();
  struct TCB* tcb = get_current_tcb();
  block(was, add_tcb, (void*)tcb);
}


void reap_tcb(void* tcb){
  spin_queue_add(&reaper_queue, (struct TCB*)tcb);
}

// terminate the current thread and free its resources.
void stop(void) {
  unsigned was = disable_interrupts();
  struct PerCore* core = get_per_core();
  struct TCB* current = core->current_thread;
  bool is_idle = (current == core->idle_thread);

  if (is_idle) {
    restore_interrupts(was);
    // idle thread should not be stopped
    event_loop();
  } else {
    // free current thread resources and block forever
    assert(n_active > 0, "no active threads to stop.\n");
    block(was, reap_tcb, (struct TCB*)current);
  }

  panic("unreachable code reached in stop().\n");
}

// block the current thread until a target jiffy count is reached.
void sleep(unsigned jiffies){
  unsigned was = disable_interrupts();
  struct TCB* tcb = get_current_tcb();
  struct PerCore* core = get_per_core();
  assert(tcb != core->idle_thread, "sleep: idle thread cannot sleep.\n");
  tcb->wakeup_jiffies = current_jiffies + jiffies;
  int args[2] = {(int)&sleep_queue, (int)tcb};
  block(was, sleep_queue_add, (void*)args);
}

bool disable_preemption(void){
  int was = disable_interrupts();
  struct TCB* tcb = get_current_tcb();
  bool prev = tcb->can_preempt;
  tcb->can_preempt = false;
  restore_interrupts(was);
  return prev;
}

void enable_preemption(bool was){
  int intrs = disable_interrupts();
  struct TCB* tcb = get_current_tcb();
  tcb->can_preempt = was;
  restore_interrupts(intrs);
}
