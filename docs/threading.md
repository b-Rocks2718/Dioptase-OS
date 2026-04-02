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


### Tests
- threads_yield.c
- threads_preempt.c
- threads_sleep.c
  