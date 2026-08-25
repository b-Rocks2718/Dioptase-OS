## Threading

### Thread Creation
To make a thread, a `struct Fun` must be allocated. The `Fun` contains the thread's function,
and the argument that will be passed to the thread. When the `Fun` is passed to `thread()`,
a TCB for the thread is created and added to the ready queue. During TCB creation, a fixed size stack is allocated per thread. The stack size is `TCB_STACK_SIZE`, which is currently set to 16384 bytes. 

### TCB Structure

The TCB stores all of the thread's state that needs to be saved on a context switch. The other info the TCB contains is if the thread is preemptable, the thread's static and MLFQ priorities,
the thread's remaining quantum, and the thread's wakeup time (for if the thread called sleep()).

### Blocking
Blocking simply context switches to the core's idle thread, and the idle thread decides 
which thread to run next. Because this can be done in O(1), it is safe to block from 
and interrupt handler. 

### Idle threads
While there are active threads, the idle threads loop forever. On each loop they call the scheduler to get the next thread to switch to, and then switch to it if it is not NULL. During
this call the scheduler does some work, such as load balancing. To avoid reentrance, the idle threads can never block. In particular they cannot use the heap.

### Freeing threads
When a thread finishes running, it is placed in a reaper queue. Because freeing threads requires acquiring the heap's blocking lock, idle threads cannot free threads that have finished running. Instead a reaper thread is created on boot, which runs with low priority and frees any threads in the reaper queue.  

`stop()` is the only producer of terminal TCBs. Before its context-switch
callback appends a TCB to the reaper queue, it disables preemption, revokes the
live TCB pointer from the child descriptor, publishes the exit result, and
releases the child's internal descriptor reference. The callback asserts that
`parent_promise` is already clear. Synchronization-object destruction never
places blocked threads on the reaper queue; owners must wake and join waiters
before quiescent destruction.

`setup_thread()` creates persistent kernel daemons which deliberately do not
contribute to the normal-thread shutdown count. Each such TCB carries an
explicit daemon marker. After every core has disabled interrupts and crossed
the shutdown barrier, core 0 detaches only marked daemons from residual global,
per-core, deferred-interrupt, pinned, and sleep queues before destroying the
scheduler's queue locks. A normal TCB in any of those queues is a lifecycle
violation and produces a diagnostic panic. Suspended daemon storage remains a
documented boot-lifetime allocation; shutdown does not try to resume or free a
kernel continuation retained by a device waiter.

Finite asynchronous work accepted by a normal TCB is counted separately from
both normal TCBs and persistent daemon TCBs. The accepting TCB calls
`kernel_async_work_begin()` before publishing the work; the daemon calls
`kernel_async_work_finish()` only after releasing every resource owned by that
work item. `event_loop()` remains live while either `n_active` or this
sequentially-consistent work count is nonzero. Consequently an asynchronous
syscall may return without letting filesystem, VM, device, or heap teardown
overtake the daemon that retained its resources. The first work reference must
come from a live normal TCB, closing the zero-to-one transition against
shutdown; persistent daemons themselves remain boot-lifetime objects.


### Tests
- threads_yield.c
- threads_preempt.c
- threads_sleep.c
