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
#include "scheduler.h"
#include "vmem.h"
#include "sys.h"
#include "promise.h"

struct SpinQueue global_ready_queue[PRIORITY_LEVELS][MLFQ_LEVELS];
struct SpinQueue reaper_queue;

int n_active = 0;
int n_active_others = 0; // number of running threads not counted in n_active
bool bootstrapping = true;

int shutdown_barrier = 0;

unsigned DEFAULT_INTERRUPT_MASK = 
  GLOBAL_INT_ENABLE | 
  SD_0_INT_ENABLE | SD_1_INT_ENABLE | 
  PIT_INT_ENABLE |
  PS2_INT_ENABLE |
  IPI_INT_ENABLE |
  AUDIO_INT_ENABLE;

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
  
  vmem_destroy_address_space(tcb);
  free_vme_list(tcb->vme_list);

  for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++){
    if (tcb->file_descriptors[i]){
      deallocate_descriptor(tcb, DESCRIPTOR_FILE, i);
    }
  }
  for (int i = 0; i < MAX_SEM_DESCRIPTORS; i++){
    if (tcb->sem_descriptors[i]){
      deallocate_descriptor(tcb, DESCRIPTOR_SEM, i);
    }
  }
  for (int i = 0; i < MAX_CHILD_DESCRIPTORS; i++){
    if (tcb->child_descriptors[i]){
      deallocate_descriptor(tcb, DESCRIPTOR_CHILD, i);
    }
  }

  node_free(tcb->cwd);
  free(tcb->cwd_path);

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
// If init_stdio is false, leave the descriptor tables empty so kernel-only
// daemon threads do not allocate stdio descriptors they can never consume.
static struct TCB* make_tcb(bool is_daemon){
  struct TCB* tcb = is_daemon ? leak(sizeof(struct TCB)) : malloc(sizeof(struct TCB));

  tcb->flags = 0;

  tcb->r20 = 0;
  tcb->r21 = 0;
  tcb->r22 = 0;
  tcb->r23 = 0;
  tcb->r24 = 0;
  tcb->r25 = 0;
  tcb->r26 = 0;
  tcb->r27 = 0;
  tcb->r28 = 0;

  tcb->imr = DEFAULT_INTERRUPT_MASK;
  tcb->pid = 0;
  tcb->fault_addr = 0;
  tcb->fault_flags = 0;
  tcb->uaccess_active = 0;
  tcb->uaccess_err_addr = 0;

  tcb->can_preempt = true;
  tcb->core_affinity = ANY_CORE;
  tcb->priority = NORMAL_PRIORITY;
  tcb->vme_list = NULL;

  tcb->cwd = &fs.root;
  tcb->cwd_path = is_daemon ? leak(2) : malloc(2);
  tcb->cwd_path[0] = '/';
  tcb->cwd_path[1] = 0;

  init_descriptors(tcb, !is_daemon);

  tcb->parent_promise = NULL;

  tcb->next = NULL;

  return tcb;
}

// create a thread to run the given function, and add it to the global ready queue
void thread(struct Fun* thread_fun){
  thread_(thread_fun, NORMAL_PRIORITY, ANY_CORE);
}


// create a thread to run the given function, and add it to the global ready queue
// allows specifying the thread's priority and the core affinity
void thread_(struct Fun* thread_fun, 
    enum ThreadPriority priority, enum CoreAffinity core_affinity){
  struct TCB* tcb = make_tcb(false);
  __atomic_fetch_add(&n_active, 1);
  __atomic_store_n(&bootstrapping, false);

  unsigned* the_stack = malloc(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ra = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->ksp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  
  tcb->priority = priority;
  tcb->core_affinity = core_affinity;
  tcb->mlfq_level = LEVEL_ZERO;
  tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];
  tcb->pid = create_page_directory();

  scheduler_wake_thread(tcb);
}

// same as thread(), but doesn't modify bootstrapping or n_active
// used to make stuff like reaper threads that won't count as active threads
// and leave the system in the bootstrapping phase
// leaks mem because it assumes these threads run forever
void setup_thread(struct Fun* thread_fun, enum ThreadPriority priority, enum CoreAffinity core_affinity){
  struct TCB* tcb = make_tcb(true);

  __atomic_fetch_add(&n_active_others, 1);

  unsigned* the_stack = leak(TCB_STACK_SIZE);
  assert(((unsigned)the_stack & 3) == 0, "stack not 4 byte aligned");
  assert(((unsigned)(&the_stack[1023]) & 3) == 0, "stack top not 4 byte aligned");
  tcb->ra = (unsigned)thread_entry;
  tcb->thread_fun = thread_fun;
  tcb->stack = the_stack;
  tcb->psr = 1; // kernel mode

  tcb->ksp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  tcb->priority = priority;
  tcb->core_affinity = core_affinity;
  tcb->mlfq_level = LEVEL_ZERO;
  tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];

  scheduler_wake_thread(tcb);
}

// initialize thread structures; should only be called once on one core
void threads_init(void){
  scheduler_init();

  shutdown_barrier = CONFIG.num_cores;

  struct Fun* reaper_fun = leak(sizeof (struct Fun));
  reaper_fun->func = (void (*)(void *))reaper;
  reaper_fun->arg = NULL;

  setup_thread(reaper_fun, LOW_PRIORITY, ANY_CORE);
}

// switch away from the current thread and run a completion callback
// Inputs: was is the interrupt mask to restore when this thread is resumed
// func/arg execute on the next context after the switch
// run_with_interrupts indicates whether to restore interrupts before running the callback
// If false, they are enabled after the callback returns
// Assumes callback doesn't modify the 'next' TCB
// Preconditions: interrupts are disabled; current thread is core->current_thread
void block(unsigned was, void (*func)(void *), void *arg, bool run_with_interrupts) {
  struct PerCore* core = get_per_core();
  struct TCB* me = core->current_thread;
  struct TCB* idle = &core->idle_thread;

  assert(me != idle, "threads block: idle thread attempted to block.\n");

  context_switch(me, idle, func, arg, &core->current_thread, was, run_with_interrupts);
}

// called when a new thread first runs
// calls the thread's main function and calls stop() when it returns
void thread_entry(void) {
  int was = interrupts_disable();
  struct TCB* current_tcb = get_current_tcb();
  interrupts_restore(was);
  struct Fun* thread_fun = current_tcb->thread_fun;
  unsigned rc = 0;
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
    rc = (*(unsigned (*)(void *))thread_fun->func)(thread_fun->arg);
  }

  // Thread cleanup:
  // stop() places thread in reaper queue
  // reaper thread eventually frees thread

  stop(rc);
}

// empty function to pass into context_switch
// when we don't need to run any callback
static void nothing(void* unused) {}

// cleanup and shutdown the system
void kernel_shutdown(void){
  interrupts_disable(); // move from interrupt-based keyboard handling to polling

  // wait for other cores to finish what they are doing
  spin_barrier_sync(&shutdown_barrier);

  // Core 0 will print results and shut down the system, 
  // other cores will wait for this to happen
  if (get_core_id() == 0) {

    // all cores are now in shutdown, so heap operations should not block
    struct GenericQueueElement* keys = blocking_queue_remove_all(&ps2_queue);
    while (keys != NULL){
      // free existing keyboard events
      struct GenericQueueElement* next = keys->next;
      free(keys);
      keys = next;
    }

    ext2_destroy(&fs);

    say("| Finished in %d jiffies\n", (int*)&current_jiffies);
    
    check_leaks();
    physmem_check_leaks();

    if (CONFIG.use_vga){
      say("| Press Q to exit...\n", NULL);

      // Wait for a full 'q' key cycle (make then break).
      // This avoids accidental exit from a stray/glitched make event.
      int saw_q_make = 0;
      while (true) {
        int key = waitkey_raw();

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

// idle thread loop
// calls to block() context switch to here, 
// where we decide which thread to run next and switch to it
void event_loop(void) {
  /* only the idle thread can enter this function */
  while (__atomic_load_n(&bootstrapping) || (__atomic_load_n(&n_active) > 0)) {
    // on each iteration, try to find a thread to switch to

    struct PerCore* core = get_per_core();
    assert(core != NULL, "per-core data is NULL.\n");
    if (core->current_thread != &core->idle_thread) {
      int args[2] = {get_core_id(), (int)core->current_thread};
      say("core %d current thread: 0x%X\n", args);
      panic("only idle thread can enter event loop.\n");
    }

    struct TCB* me = core->current_thread;
    struct TCB* next = schedule_next_thread();

    int was = interrupts_disable();
    if (next != NULL) {
      context_switch(me, next, nothing, NULL, &core->current_thread, was, true);
    } else {
      interrupts_restore(was);

      // put core to sleep to save power until the next interrupt if there's no work to do
      pause();
    }
  }

  kernel_shutdown();
}

// set up thread context for the first thread on this core (which is now the idle thread)
void bootstrap(void){
  int imr = get_imr();
  assert((imr & GLOBAL_INT_ENABLE) == 0, 
    "interrupts should be disabled when bootstrapping thread context.\n");

  int me = get_core_id();
  struct PerCore* core = get_per_core();
  struct TCB* tcb = &core->idle_thread;

  tcb->flags = 0;

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
  tcb->ra = 0;
  tcb->psr = 1;
  tcb->imr = 0;
  tcb->fault_addr = 0;
  tcb->fault_flags = 0;
  tcb->uaccess_active = 0;
  tcb->uaccess_err_addr = 0;

  tcb->stack = (unsigned*)(IDLE_STACKS_TOP - (me * IDLE_STACK_SIZE));

  core->current_thread = tcb;
}

// voluntarily yield the CPU and re-queue the current thread
void yield(void){
  unsigned was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  scheduler_charge_yield(tcb);
  block(was, local_queue_add, (void*)tcb, true);
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
  block(was, sleep_queue_add, (void*)args, true);
}

// terminate the current thread and 
// place it on the reaper queue to eventually free its resources
void stop(unsigned rc) {
  unsigned was = interrupts_disable();
  struct PerCore* core = get_per_core();
  struct TCB* current = core->current_thread;
  interrupts_restore(was);
  bool is_idle = (current == &core->idle_thread);

  if (current->parent_promise != NULL){
    promise_set(current->parent_promise->child, (void*)rc);
  }

  if (__atomic_fetch_add(&current->parent_promise->refcount, -1) == 1){
    promise_free(current->parent_promise->child);
    free(current->parent_promise);
  }

  was = interrupts_disable();

  if (is_idle) {
    panic("idle thread cannot call stop().\n");
  } else {
    // free current thread resources and block forever
    assert(n_active > 0, "no active threads to stop.\n");
    block(was, reap_tcb, (struct TCB*)current, true);
  }

  panic("unreachable code reached in stop().\n");
}

// disable preemption and return whether it was previously enabled or not
bool preemption_disable(void){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();

  // no-op if threading not initialized yet
  if (tcb == NULL) {
    interrupts_restore(was);
    return false;
  }

  bool prev = tcb->can_preempt;
  tcb->can_preempt = false;
  interrupts_restore(was);
  return prev;
}

// restore preemption to the given value
void preemption_restore(bool was){
  int intrs = interrupts_disable();
  struct TCB* tcb = get_current_tcb();

  // no-op if threading not initialized yet
  if (tcb != NULL) {
    tcb->can_preempt = was;
  }

  interrupts_restore(intrs);
}

// pin a thread to the current core, preventing it from being scheduled on other cores
enum CoreAffinity core_pin(void){
  int was = interrupts_disable();
  unsigned me = get_core_id();
  struct TCB* tcb = get_current_tcb();

  // no-op if threading not initialized yet
  if (tcb == NULL) {
    interrupts_restore(was);
    return ANY_CORE;
  }

  enum CoreAffinity prev = tcb->core_affinity;
  tcb->core_affinity = me;
  interrupts_restore(was);

  return prev;
}

// allow a thread to be scheduled on any core
void core_unpin(enum CoreAffinity prev){
  int was = interrupts_disable();
  unsigned me = get_core_id();
  struct TCB* tcb = get_current_tcb();

  // no-op if threading not initialized yet
  if (tcb == NULL) {
    interrupts_restore(was);
    return;
  }

  tcb->core_affinity = prev;
  interrupts_restore(was);
}
