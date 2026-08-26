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
#include "audio.h"
#include "physmem.h"
#include "sd_driver.h"

// jump_to_user executes one rfe, so this is the only PSR depth from which a
// final kernel-return path can enter a user signal handler in user mode.
#define USER_RETURNABLE_PSR_DEPTH 1

/*
 * Reserve one speculative fetch-add slot per hardware core. At most MAX_CORES
 * callers can enter begin concurrently, so even when every caller rejects and
 * rolls back at this threshold, the signed counter cannot overflow. The
 * bootstrap compiler requires a literal global constant; threads_init()
 * verifies this remains INT_MAX - MAX_CORES.
 */
#define KERNEL_ASYNC_WORK_LIMIT 0x7FFFFFFB

struct SpinQueue global_ready_queue[PRIORITY_LEVELS][MLFQ_LEVELS];
struct SpinQueue reaper_queue;

int n_active = 0;
int n_active_others = 0; // number of running threads not counted in n_active
/*
 * Work accepted by a normal TCB but completed by a persistent daemon. Unlike
 * n_active_others, this counts finite resource-owning operations rather than
 * the boot-lifetime daemon TCBs themselves.
 */
static int kernel_async_work_count = 0;
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

// Publish one child exit and revoke its TCB from every descriptor holder.
//
// Atomicity: state_lock serializes child_tcb and pending-signal access across
// cores.  Once this function clears child_tcb, no sender can obtain the TCB;
// only then may the caller enqueue it for reaping.
static void publish_exit(struct TCB* child, unsigned rc) {
  struct ChildDescriptor* descriptor = child->parent_promise;
  if (descriptor == NULL){
    return;
  }

  clh_lock_acquire(&descriptor->state_lock);
  assert(descriptor->child_tcb == child,
    "child exit: descriptor does not reference exiting TCB.\n");
  descriptor->child_tcb = NULL;
  clh_lock_release(&descriptor->state_lock);

  promise_set(descriptor->child_promise, (void*)rc);
  child->parent_promise = NULL;
  child_descriptor_release(descriptor);
}

struct SignalDelivery {
  void* handler;
  int signal;
};

static bool has_unhandled_signal(struct TCB* child,
    struct SignalDelivery* delivery) {
  struct ChildDescriptor* descriptor = child->parent_promise;
  if (descriptor == NULL){
    return false;
  }

  clh_lock_acquire(&descriptor->state_lock);
  bool terminate = false;
  assert((child->signal_mask & ((1u << MAX_MASKABLE_SIGNAL) - 1)) == child->signal_mask,
    "child signal mask has bits set beyond MAX_MASKABLE_SIGNAL.\n");

  unsigned current_signals = child->pending_signals & (~child->signal_mask);
  
  unsigned nonmaskable_signals =
    current_signals & ~((1u << MAX_MASKABLE_SIGNAL) - 1);
  bool kill_signal = (current_signals & (1u << SIGNAL_KILL)) != 0;

  bool in_signal_handler = child->in_signal_handler;

  if (kill_signal){
    // cannot mask or handle kill signal
    terminate = true;
  } else if (in_signal_handler) {
    if (nonmaskable_signals != 0){
      // nonmaskable signals cannot be handled while in a signal handler
      terminate = true;
    }
    // maskable signals can wait until we exit this handler
  } else {
    // not in a signal handler
    if (current_signals != 0){
      // check for a signal handler to run
      for (int i = 0; i < MAX_SIGNALS; i++){
        if ((current_signals & (1u << i))){
          if (child->signal_handlers[i]) {
            // found a handler we can use
            assert(delivery != NULL,
              "has_unhandled_signal: delivery pointer is NULL.\n");
            delivery->handler = child->signal_handlers[i];
            delivery->signal = i;

            // clear the signal from pending_signals so we don't run it again
            child->pending_signals &= ~(1u << i);

            break;
          } else {
            // no handler and signal is not masked -> terminate
            terminate = true;
            break;
          }
        }
      }
    }
  }

  clh_lock_release(&descriptor->state_lock);
  return terminate;
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

  free(tcb->my_node);

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

  // Daemon threads never enter user mode.  ChildDescriptor.state_lock, rather
  // than a TCB-local lock, protects the signal state of user threads.
  if (!is_daemon) {
    tcb->pending_signals = 0;
    tcb->signal_mask = 0;
    for (int i = 0; i < MAX_SIGNALS; i++){
      tcb->signal_handlers[i] = NULL;
    }
    tcb->in_signal_handler = false;
  }
  tcb->signal_stack_top = 0;

  tcb->my_node = is_daemon ? leak(sizeof(struct CLHNode)) : malloc(sizeof(struct CLHNode));
  tcb->my_node->locked = false;
  tcb->my_node->interrupt_state = 0;
  tcb->my_pred = NULL;

  tcb->is_daemon = is_daemon;
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
  assert(tcb->pid != 0,
    "thread: failed to allocate a page directory for a new kernel thread.\n");

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

void kernel_async_work_begin(void){
  struct TCB* current = get_current_tcb();
  int active_threads = __atomic_load_n(&n_active);

  if (current == NULL || current->is_daemon || active_threads <= 0){
    int args[3] = {(int)current,
      current == NULL ? -1 : current->is_daemon,
      active_threads};
    say("| async work begin rejected tcb=0x%X daemon=%d n_active=%d\n",
      args);
    panic("async work begin: first publication must be owned by a live normal thread.\n");
  }

  int previous = __atomic_fetch_add(&kernel_async_work_count, 1);
  if (previous < 0 || previous >= KERNEL_ASYNC_WORK_LIMIT){
    __atomic_fetch_add(&kernel_async_work_count, -1);
    int args[2] = {previous, KERNEL_ASYNC_WORK_LIMIT};
    say("| async work begin rejected previous=%d limit=%d\n", args);
    panic("async work begin: outstanding-work count is invalid or exhausted.\n");
  }
}

void kernel_async_work_finish(void){
  int previous = __atomic_fetch_add(&kernel_async_work_count, -1);
  if (previous <= 0){
    __atomic_fetch_add(&kernel_async_work_count, 1);
    int args[1] = {previous};
    say("| async work finish rejected previous=%d\n", args);
    panic("async work finish: no outstanding work reference exists.\n");
  }
}

// initialize thread structures; should only be called once on one core
void threads_init(void){
  if (KERNEL_ASYNC_WORK_LIMIT != INT_MAX - MAX_CORES){
    int args[3] = {KERNEL_ASYNC_WORK_LIMIT, INT_MAX, MAX_CORES};
    say("| threads init rejected async_limit=%d int_max=%d max_cores=%d\n",
      args);
    panic("threads init: asynchronous-work limit must reserve one RMW slot per core.\n");
  }

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

  assert_always(me->my_node->locked == false, "threads block: thread tried to block while holding a spinlock.\n");

  assert_always(me != idle, "threads block: idle thread attempted to block.\n");

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

// Enter a user signal handler on the thread's dedicated signal stack.
//
// Preconditions:
// - The caller executes in kernel mode on behalf of the currently installed,
//   non-idle user TCB. That TCB's PID and kernel stack are active on this core.
// - handler names executable memory in that TCB's user address space.
// - signal_stack_top names the top of the current image's writable signal
//   stack and no signal handler is already active.
//
// Postconditions:
// - sigreturn() clears in_signal_handler and resumes this kernel activation,
//   allowing the interrupted user context to be restored by its trap or
//   exception wrapper.
// - exit(), a handler fault, or a normal C return leaves in_signal_handler set;
//   the current TCB is then published as exited and is never rescheduled.
//
// Concurrency and CPU state:
// - The current TCB cannot execute concurrently on another core. Final-return
//   delivery and current-thread exception delivery therefore own
//   in_signal_handler and signal_handlers without ChildDescriptor.state_lock.
// - Signals sent from other cores mutate pending_signals under state_lock and
//   are observed by a later final user-return path after this handler
//   completes. The
//   architecture memory model is sequentially consistent.
// - jump_to_user performs the kernel-to-user transition and preserves the
//   kernel continuation on the TCB's current kernel stack. Because
//   jump_to_user replaces architectural r31 with the signal-stack pointer, the
//   original user r31 is explicitly restored before the suspended context
//   continues.
// - PSR must be exactly 1. jump_to_user executes one rfe, so a larger nesting
//   depth would leave the handler in kernel mode, where r31 aliases KSP rather
//   than the user signal-stack pointer.
static void run_user_signal_handler(void* handler, unsigned arg1,
    unsigned arg2) {
  struct TCB* me = get_current_tcb();
  assert(me != NULL,
    "signal delivery: current TCB is NULL while entering user handler.\n");
  assert(!me->in_signal_handler,
    "signal delivery: attempted nested user signal handler entry.\n");

  // Make handler activation atomic with respect to PIT return processing. If
  // interrupts remained enabled between selecting a signal and setting
  // in_signal_handler, a PIT could select a second signal in that window and
  // create a nested handler despite the no-nesting contract.
  unsigned caller_imr = interrupts_disable();
  unsigned saved_user_sp = get_user_sp();
  me->in_signal_handler = true;
  assert(get_cr0() == USER_RETURNABLE_PSR_DEPTH,
    "signal delivery: user handler entry requires PSR depth 1.\n");

  /*
   * interrupts_disable() clears the complete IMR, including every individual
   * device-enable bit. Restore the caller's lower mask bits before rfe, but
   * keep IMR[31] clear so no interrupt can enter between publishing
   * in_signal_handler and jump_to_user saving its kernel continuation. Per the
   * ISA, rfe sets the global-enable bit while retaining these lower bits.
   */
  interrupts_restore(caller_imr & ~GLOBAL_INT_ENABLE);
  int rc = jump_to_user((unsigned)handler, me->signal_stack_top, arg1, arg2);

  // A handler returns here through a nested sigreturn trap. That trap's
  // return-to-kernel path leaves interrupts enabled, but the suspended outer
  // wrapper may have entered with interrupts disabled (notably PIT). Prevent
  // another interrupt from observing the handler's user SP, restore the
  // suspended user SP, then re-establish the caller's exact IMR.
  interrupts_disable();
  set_user_sp(saved_user_sp);
  interrupts_restore(caller_imr);
  if (me->in_signal_handler) {
    // The handler did not call sigreturn(). This includes an explicit exit,
    // a user exception that aborted its nested jump_to_user(), and a normal
    // return through an undefined return address on the fresh signal stack.
    stop(rc);
  }
}

bool try_run_current_signal_handler(int signal, unsigned arg1, unsigned arg2) {
  if (signal < 0 || signal >= MAX_SIGNALS){
    return false;
  }

  struct TCB* me = get_current_tcb();
  if (me == NULL || me->in_signal_handler ||
      me->signal_handlers[signal] == NULL){
    return false;
  }

  run_user_signal_handler(me->signal_handlers[signal], arg1, arg2);
  return true;
}

// Linearize sigreturn with cross-core signal publication.
//
// Preconditions:
// - The current non-idle user TCB is executing the TRAP_SIGRETURN continuation
//   in kernel mode, and in_signal_handler is true.
// - No spinlock is held on entry. Interrupts may be enabled by trap entry.
//
// Postconditions:
// - If a nonmaskable signal was published before state_lock is acquired, the
//   current TCB terminates through stop() and this function does not return.
// - Otherwise in_signal_handler becomes false while holding the same
//   state_lock used by cross-core senders. A sender ordered after that clear is
//   a post-handler send and leaves its bit pending for a later final return.
// - The caller's exact IMR is restored on the returning path.
void finish_current_signal_handler(void) {
  unsigned caller_imr = interrupts_disable();
  struct TCB* me = get_current_tcb();
  assert(me != NULL,
    "signal return: current TCB is NULL while completing sigreturn.\n");
  assert(me->in_signal_handler,
    "signal return: sigreturn completion requires an active handler.\n");

  bool terminate = false;
  struct ChildDescriptor* descriptor = me->parent_promise;
  if (descriptor != NULL) {
    clh_lock_acquire(&descriptor->state_lock);
    unsigned eligible = me->pending_signals & ~me->signal_mask;
    unsigned maskable_bits = (1u << MAX_MASKABLE_SIGNAL) - 1;
    terminate = (eligible & ~maskable_bits) != 0;
    if (!terminate) {
      me->in_signal_handler = false;
    }
    clh_lock_release(&descriptor->state_lock);
  } else {
    // The initial process has no parent descriptor and therefore no external
    // signal sender. It still uses the same interrupt-atomic state transition.
    me->in_signal_handler = false;
  }

  if (terminate) {
    stop((unsigned)-1);
  }

  interrupts_restore(caller_imr);
}

// Consume at most one asynchronous signal at a true final kernel-to-user
// boundary.
//
// Preconditions:
// - The current thread's syscall/fault/interrupt continuation has completed;
//   it holds no spinlock, has accepted every wakeup handoff, and is
//   preemptible.
// - The wrapper still owns the saved user register/EPC/EFG frame and will
//   restore it only after this function returns.
// - cr0 is exactly 1, so jump_to_user's one rfe enters user mode.
//
// Postconditions:
// - No eligible signal: the pending bitmap is unchanged.
// - Handled signal: one lowest-numbered pending bit is consumed and the
//   handler has completed through sigreturn before this function returns.
// - Default action or SIGNAL_KILL: stop() publishes exit and this function
//   never returns.
//
// Ordering: ChildDescriptor.state_lock serializes the pending bitmap with
// cross-core senders. The current TCB exclusively owns its mask, handler table,
// and in_signal_handler state under the sequentially-consistent memory model.
void process_pending_signals_before_user_return(void) {
  assert(get_cr0() == USER_RETURNABLE_PSR_DEPTH,
    "signal return: final user return requires PSR depth 1.\n");

  // Keep interrupts disabled from pending-bit selection through either
  // handler activation or the no-delivery decision. This closes the interval
  // in which a PIT return could otherwise select a second signal before
  // in_signal_handler becomes visible.
  unsigned caller_imr = interrupts_disable();
  struct PerCore* core = get_per_core();
  struct TCB* me = core->current_thread;

  assert(me != NULL,
    "signal return: current TCB is NULL at final user return.\n");
  assert(me != &core->idle_thread,
    "signal return: idle thread cannot return to user mode.\n");
  assert(me->can_preempt,
    "signal return: final user return reached with preemption disabled.\n");
  assert_always(me->my_node != NULL && !me->my_node->locked,
    "signal return: final user return reached while holding a spinlock.\n");

  struct SignalDelivery delivery = {NULL, -1};
  if (has_unhandled_signal(me, &delivery)) {
    stop((unsigned)-1);
  }

  if (delivery.handler != NULL) {
    /*
     * has_unhandled_signal() ran with the complete IMR cleared. Reinstall the
     * final-return caller's device mask with global delivery still disabled so
     * run_user_signal_handler() can carry those bits through its own atomic
     * activation window and into rfe.
     */
    interrupts_restore(caller_imr & ~GLOBAL_INT_ENABLE);
    run_user_signal_handler(delivery.handler, delivery.signal, 0);
  }

  interrupts_restore(caller_imr);
}

// cleanup and shutdown the system
void kernel_shutdown(void){
  interrupts_disable(); // move from interrupt-based keyboard handling to polling

  // wait for other cores to finish what they are doing
  spin_barrier_sync(&shutdown_barrier);

  // Core 0 will print results and shut down the system, 
  // other cores will wait for this to happen
  if (get_core_id() == 0) {
    ext2_destroy(&fs);
    ps2_destroy();
    audio_destroy();
    trap_destroy();
    sd_destroy();
    vmem_global_destroy();
    scheduler_destroy();
    physmem_destroy_locks();
    heap_destroy();

    say("| Finished in %d jiffies\n", (int*)&current_jiffies);
    
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
  while (__atomic_load_n(&bootstrapping) ||
      (__atomic_load_n(&n_active) > 0) ||
      (__atomic_load_n(&kernel_async_work_count) > 0)) {
    // on each iteration, try to find a thread to switch to

    struct PerCore* core = get_per_core();
    assert_always(core != NULL, "per-core data is NULL.\n");
    if (core->current_thread != &core->idle_thread) {
      int args[2] = {get_core_id(), (int)core->current_thread};
      say("core %d current thread: 0x%X\n", args);
      panic("only idle thread can enter event loop.\n");
    }

    struct TCB* me = core->current_thread;
    struct TCB* next = schedule_next_thread();

    if (next == NULL) {
      // no work to do
      pause();
      continue;
    }

    // The scheduler cannot infer a safe user-return boundary from saved PSR.
    // A just-woken TCB may still be suspended inside a syscall/fault after a
    // semaphore or lock handoff. Resume its kernel continuation unchanged;
    // the final trap/interrupt/exception epilogue processes pending signals
    // only after that continuation has released its resources.
    int was = interrupts_disable();
    context_switch(me, next, nothing, NULL, &core->current_thread, was, true);
  }

  kernel_shutdown();
}

// set up thread context for the first thread on this core (which is now the idle thread)
void bootstrap(void){
  int imr = get_imr();
  assert_always((imr & GLOBAL_INT_ENABLE) == 0,
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
  tcb->is_daemon = true;
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

  assert_always(me >= 0 && me < MAX_CORES, "bootstrap: core id is outside idle CLH node table.\n");
  assert_always(per_core_data[me].idle_clh_node != NULL, "bootstrap: idle CLH node table was not initialized.\n");
  tcb->my_node = per_core_data[me].idle_clh_node;
  tcb->my_pred = NULL;

  // idle threads should never enter user mode
  // so skip setting up signal handling 
  // (avoids a call to malloc() to create the CLH lock)
  tcb->signal_stack_top = 0;

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
static void reap_tcb(void* tcb){
  struct TCB* terminal = (struct TCB*)tcb;
  assert(terminal != NULL, "thread reap: terminal TCB is NULL.\n");

  /*
   * Preconditions: this callback runs after stop() has disabled preemption,
   * published the child result, revoked ChildDescriptor.child_tcb, and
   * context-switched permanently away from terminal. No other subsystem may
   * manufacture a terminal transition by placing a blocked TCB here.
   *
   * Postcondition: reaper_queue is the sole owner responsible for freeing the
   * TCB. parent_promise == NULL is the locally checkable proof that any child
   * descriptor and wait_child() promise were handled before publication.
   */
  assert(terminal->parent_promise == NULL,
    "thread reap: TCB still has a parent descriptor; termination bypassed stop()/exit publication.\n");
  spin_queue_add(&reaper_queue, terminal);
}

// Block the current thread until a target jiffy count is reached.
//
// Preconditions: kernel mode in a non-idle TCB; jiffies is at most INT_MAX.
// The half-range limit makes modular deadline ordering unambiguous even when
// current_jiffies + jiffies wraps through zero. User trap arguments are checked
// before entering this kernel primitive.
void sleep(unsigned jiffies){
  assert(jiffies <= INT_MAX,
    "sleep: duration exceeds the supported INT_MAX-jiffy horizon.\n");
  unsigned was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  struct PerCore* core = get_per_core();
  assert_always(tcb != &core->idle_thread, "sleep: idle thread cannot sleep.\n");
  tcb->wakeup_jiffies = current_jiffies + jiffies;
  int args[2] = {(int)&core->sleep_queue, (int)tcb};
  block(was, sleep_queue_add, (void*)args, true);
}

// Terminate the current thread and place it on the reaper queue.
//
// Preconditions:
// - This runs in kernel mode in the current non-idle TCB's context.
// - The current TCB does not hold a spinlock and must never run again.
//
// Concurrency and CPU state:
// - Preemption remains disabled from before exit publication through the final
//   context switch. Interrupts may still run, but the PIT handler must not put
//   this terminal TCB back on a ready queue between revoking child_tcb and
//   blocking it forever.
// - publish_exit() serializes descriptor and pending-signal state with other
//   cores through ChildDescriptor.state_lock.
//
// Postcondition: this function does not return; the reaper owns current.
void stop(unsigned rc) {
  // There is intentionally no matching preemption_restore(): after terminal
  // state becomes externally visible, this TCB may never be scheduled again.
  preemption_disable();

  unsigned was = interrupts_disable();
  struct PerCore* core = get_per_core();
  struct TCB* current = core->current_thread;
  interrupts_restore(was);
  bool is_idle = (current == &core->idle_thread);

  publish_exit(current, rc);

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
