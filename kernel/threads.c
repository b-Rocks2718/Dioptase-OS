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

struct SpinQueue global_ready_queue;
struct SpinQueue reaper_queue;

int n_active = 0;
int n_active_others = 0; // number of running threads not counted in n_active
bool bootstrapping = true;

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

// reaper thread that runs forever and frees resources of threads that have been stopped
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

// return a TCB struct
// defaults to: preemption enabled, not pinned, normal priority
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
  tcb->imr = 0x80000001;

  tcb->can_preempt = true;
  tcb->core_affinity = ANY_CORE;
  tcb->priority = NORMAL_PRIORITY;

  tcb->next = NULL;

  return tcb;
}

// create a thread to run the given function, and add it to the global ready queue
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
  
  global_queue_add(tcb);
}

// same as thread(), but doesn't modify bootstrapping or n_active
// used to make stuff like reaper threads that won't count as active threads
// and leave the system in the bootstrapping phase
// leaks mem because it assumes these threads run forever
static void setup_thread(struct Fun* thread_fun){
  struct TCB* tcb = make_tcb(true);

  __atomic_fetch_add(&n_active_others, 1);

  unsigned* the_stack = leak(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ret_addr = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->sp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  
  global_queue_add(tcb);
}

// initialize scheduler structures; should only be called once on one core
void threads_init(void){
  spin_queue_init(&global_ready_queue);
  spin_queue_init(&reaper_queue);

  for (int i = 0; i < MAX_CORES; i++) {
    queue_init(&per_core_data[i].ready_queue);
    sleep_queue_init(&per_core_data[i].sleep_queue);
    spin_queue_init(&per_core_data[i].pinned_queue);
  }

  struct Fun* reaper_fun = leak(sizeof (struct Fun));
  reaper_fun->func = (void (*)(void *))reaper;
  reaper_fun->arg = NULL;

  setup_thread(reaper_fun);
}

// switch away from the current thread and run a completion callback
// Inputs: was is the interrupt mask to restore when this thread is resumed
// func/arg execute on the next context after the switch
// Preconditions: interrupts are disabled; current thread is core->current_thread
void block(unsigned was, void (*func)(void *), void *arg) {
  struct PerCore* core = get_per_core();
  struct TCB* me = core->current_thread;
  struct TCB* idle = &core->idle_thread;

  context_switch(me, idle, func, arg, &core->current_thread, was);
}

// called when a new thread first runs
// calls the thread's main function and calls stop() when it returns
void thread_entry(void) {
  int was = interrupts_disable();
  struct TCB* current_tcb = get_current_tcb();
  interrupts_restore(was);
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

// empty function to pass into context_switch
// when we don't need to run any callback
static void nothing(void* unused) {}

#define GLOBAL_CHECK_INTERVAL 8 // every 8 iterations of the event loop, check the global queue for new work
#define REBALANCE_INTERVAL 32 // only check for rebalancing every 32 iterations to avoid excessive overhead
#define MAX_REBALANCE_PERCENT 130 // rebalance if we have >130% of our ideal number of threads
#define MIN_REBALANCE_PERCENT 70 // rebalance if we have <70% of our ideal number of threads

// idle thread loop
// calls to block() context switch to here, 
// where we decide which thread to run next and switch to it
void event_loop(void) {
  /* only the idle thread can enter this function */
  unsigned event_loop_iters = 0;
  while (__atomic_load_n(&bootstrapping) || (__atomic_load_n(&n_active) > 0)) {
    struct PerCore* core = get_per_core();
    
    // check sleep queue
    struct TCB* wakeup = sleep_queue_remove(&core->sleep_queue);
    while (wakeup != NULL) {
      queue_add(&core->ready_queue, wakeup);
      wakeup = sleep_queue_remove(&core->sleep_queue);
    }

    // empty pinned queue into local ready queue
    struct TCB* pinned = spin_queue_remove_all(&core->pinned_queue);
    while (pinned != NULL) {
      struct TCB* next = pinned->next;
      pinned->next = NULL;
      queue_add(&core->ready_queue, pinned);
      pinned = next;
    }

    if (event_loop_iters % REBALANCE_INTERVAL == 0) {
      // work out if our local queue has too many or too few threads

      // Use ceiling division so remainder runnable threads do not remain stuck
      // on the global queue when the active count is not divisible by core count.
      unsigned total_active = __atomic_load_n(&n_active) + __atomic_load_n(&n_active_others);
      unsigned ideal = (total_active + CONFIG.num_cores - 1) / CONFIG.num_cores;
      unsigned local_size = __atomic_load_n(&core->ready_queue.size);
      unsigned global_size = __atomic_load_n(&global_ready_queue.size);

      if (local_size * 100 > ideal * MAX_REBALANCE_PERCENT) {
        // if we have > 130% of our ideal number of threads, move some to the global queue
        int to_move = local_size - ideal;
        unsigned scan_budget = local_size;
        int moved = 0;
        while (moved < to_move && scan_budget > 0) {
          struct TCB* tcb = queue_remove(&core->ready_queue);
          if (tcb == NULL) break;
          if (tcb->core_affinity != ANY_CORE) {
            // Keep pinned threads on their home core's local queue.
            queue_add(&core->ready_queue, tcb);
          } else {
            global_queue_add(tcb);
            moved++;
          }
          scan_budget--;
        }
      } else if (local_size * 100 < ideal * MIN_REBALANCE_PERCENT) {
        // if we have < 70% of our ideal number of threads, try to take some from the global queue
        int to_move = ideal - local_size;
        for (int i = 0; i < to_move; i++) {
          struct TCB* tcb = spin_queue_remove(&global_ready_queue);
          if (tcb == NULL) break;
          queue_add(&core->ready_queue, tcb);
        }
      }
    }

    struct TCB* next = NULL;

    // Periodically check global work first so threads woken onto the global
    // queue cannot starve behind a permanently "balanced" local queue set.
    if ((event_loop_iters % GLOBAL_CHECK_INTERVAL) == 0) {
      next = spin_queue_remove(&global_ready_queue);
    }

    if (next == NULL) {
      // if no global work (or we didn't check), run local work
      next = queue_remove(&core->ready_queue);
    }

    if (next == NULL) {
      // if no local work, steal from global queue
      next = spin_queue_remove(&global_ready_queue);
    }

    assert(core != NULL, "per-core data is NULL.\n");
    if (core->current_thread != &core->idle_thread) {
      int args[2] = {get_core_id(), (int)core->current_thread};
      say("core %d current thread: 0x%X\n", args);
      panic("only idle thread can enter event loop.\n");
    }

    struct TCB* me = core->current_thread;

    // if we actually are an idle thread, preemption is disabled
    // and we don't actually need to disable interrupts here
    // this is just for debugging
    int was = interrupts_disable();

    if (next != NULL) {
      context_switch(me, next, nothing, NULL, &core->current_thread, was);
    } else {
      interrupts_restore(was);

      // put core to sleep to save power until the next interrupt if there's no work to do
      pause();
    }

    event_loop_iters++;
  }

  // Core 0 will print results and shut down the system, 
  // other cores will wait for this to happen
  if (get_core_id() == 0) {
    say("| Finished in %d jiffies\n", (int*)&current_jiffies);

    say("| Checking leaks\n", NULL);
    
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

// set up thread context for the first thread on this core (which is now the idle thread)
void bootstrap(void){
  // interrupts should be disabled when calling this function
  // but we'll be safe
  int was = interrupts_disable();
  int me = get_core_id();
  struct TCB* tcb = &get_per_core()->idle_thread;

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
  tcb->core_affinity = me;
  tcb->priority = NORMAL_PRIORITY;
  // these values should never be used
  tcb->thread_fun = NULL;
  tcb->bp = 0;
  tcb->sp = 0;
  tcb->ret_addr = 0;
  tcb->psr = 1;
  tcb->imr = 0;

  tcb->stack = (unsigned*)(0x10000 - (me * 0x4000));

  struct PerCore* core = get_per_core();
  assert((was & 0x80000000) == 0, 
    "interrupts should be disabled when bootstrapping thread context.\n");
  core->current_thread = tcb;
  interrupts_restore(was);
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

// add a thread to the core-local ready queue
void local_queue_add(void* tcb){
  int was = interrupts_disable();
  // leave interrupts disabled around queue_add to avoid re-entrancy issues
  queue_add(&get_per_core()->ready_queue, (struct TCB*)tcb);
  interrupts_restore(was);
}

// voluntarily yield the CPU and re-queue the current thread
void yield(void){
  unsigned was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  block(was, local_queue_add, (void*)tcb);
}

// add a thread to the reaper queue to have its resources freed by the reaper thread
void reap_tcb(void* tcb){
  spin_queue_add(&reaper_queue, (struct TCB*)tcb);
}

// block the current thread until a target jiffy count is reached
void sleep(unsigned jiffies){
  unsigned was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  struct PerCore* core = get_per_core();
  assert(tcb != &core->idle_thread, "sleep: idle thread cannot sleep.\n");
  tcb->wakeup_jiffies = current_jiffies + jiffies;
  int args[2] = {(int)&core->sleep_queue, (int)tcb};
  block(was, sleep_queue_add, (void*)args);
}

// terminate the current thread and 
// place it on the reaper queue to eventually free its resources
void stop(void) {
  unsigned was = interrupts_disable();
  struct PerCore* core = get_per_core();
  struct TCB* current = core->current_thread;
  bool is_idle = (current == &core->idle_thread);

  if (is_idle) {
    panic("idle thread cannot call stop().\n");
  } else {
    // free current thread resources and block forever
    assert(n_active > 0, "no active threads to stop.\n");
    block(was, reap_tcb, (struct TCB*)current);
  }

  panic("unreachable code reached in stop().\n");
}

// disable preemption and return whether it was previously enabled or not
bool preemption_disable(void){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  bool prev = tcb->can_preempt;
  tcb->can_preempt = false;
  interrupts_restore(was);
  return prev;
}

// restore preemption to the given value
void preemption_restore(bool was){
  int intrs = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  tcb->can_preempt = was;
  interrupts_restore(intrs);
}

// pin a thread to the current core, preventing it from being scheduled on other cores
void core_pin(void){
  int was = interrupts_disable();
  unsigned me = get_core_id();
  struct TCB* tcb = get_current_tcb();
  tcb->core_affinity = me;
  interrupts_restore(was);
}

// allow a thread to be scheduled on any core
void core_unpin(void){
  int was = interrupts_disable();
  unsigned me = get_core_id();
  struct TCB* tcb = get_current_tcb();
  tcb->core_affinity = ANY_CORE;
  interrupts_restore(was);
}
