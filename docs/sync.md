## Synchronization Primitives

The semaphore and rwlock implementations touch threading logic directly because they call `block()`. Everything else is layered on top of those primitives plus the atomic primitives.

### Supported Sync Primitives

#### Disable/Restore Interrupts
Saves the current IMR, disables interrupts, and later restores the previous IMR. `interrupts_disable()` returns whether preemption was previously enabled, and `interrupts_restore()` restores that saved state.

Exercised by `atomic_test.c`

#### Disable/Restore Preemption
Writes to the `can_preempt` field of the current thread's TCB, with interrupts briefly disabled so the update is atomic with respect to the PIT handler. `preemption_disable()` returns whether preemption was previously enabled, and `preemption_restore()` restores that saved state.

Tested in `threads_preempt_toggle.c`

#### Core Pinning
Writes to the `core_affinity` field of the current thread's TCB. `core_pin()` pins the thread to the current core, and `core_unpin()` restores to the old core affinity. The scheduler respects this field when choosing where a thread may run.

Tested in `threads_core_pin.c`

#### Spinlock
Uses atomic exchange until it acquires the lock. It disables interrupts before each attempt, and restores interrupts if an attempt fails. If the lock is acquired, interrupts remain disabled until the lock is released. For this reason, regions protected by a spinlock should be O(1).

Tested in `atomic_test.c`

#### Preemption Spinlock
Same as spinlock, but disables preemption on each acquire attempt instead of interrupts. This is for cases where the critical section cannot be assumed to be O(1), but the caller still cannot block. The current use case is the print lock, so even idle threads can safely serialize debugging output.

No dedicated OS test currently exercises this primitive directly.

#### Spin Barrier
Simple one-shot barrier for a known number of participants. Each thread decrements a shared counter and then spins until the counter reaches 0. It does not reset itself, so the caller must reinitialize the counter before reusing it.

Tested in `atomic_test.c`

#### Semaphore
Counting semaphore protected by a spinlock. `sem_down()` consumes a permit immediately if one is available; otherwise it blocks the current thread until a later `sem_up()`. `sem_up()` wakes exactly one waiter if any are queued, or increments the count if nobody is waiting. Destroying a semaphore reaps blocked waiters instead of waking them back into execution.

Tested in `threads_semaphore.c` and `threads_semaphore_destroy_cleanup.c`

#### RwLock
Write-preferring reader-writer lock. Multiple readers may hold it at once, but only when there is no active writer and no queued writer. Writers acquire exclusive access. On release, a writer hands off to another waiting writer first; only when no writers are waiting are blocked readers released.

Tested in `threads_rw_lock.c`

#### Blocking Lock
Mutex-style lock implemented as a `Semaphore(1)`. Acquiring it may block, so unlike a spinlock it is suitable for longer critical sections. A successful acquire disables preemption and saves the caller's prior preemption state. Release wakes the next waiter and then restores that saved preemption state.

Tested in `threads_cond_var.c`, `threads_barrier.c`, `threads_gate.c`, and `threads_event.c`

#### Promise
One-shot publication primitive implemented with a semaphore. `promise_set()` stores a pointer and opens the promise. `promise_get()` blocks until the first set, then reposts the semaphore so every later getter also returns immediately with the same pointer.

Tested in `threads_promise.c`

#### Bounded Buffer
Fixed-capacity blocking queue of `GenericQueueElement`s. One semaphore counts free slots and another counts queued items. `bounded_buffer_add()` blocks while the buffer is full, and `bounded_buffer_remove()` blocks while it is empty. There are also non-blocking `remove_all()` and size helpers.

Tested in `threads_bounded_buffer.c`

#### Blocking RingBuf
Fixed-capacity blocking FIFO of `char` bytes backed by owned ring storage. One semaphore counts free byte slots and another counts queued bytes. `blocking_ringbuf_add()` blocks while the ring is full, and `blocking_ringbuf_remove()` blocks while it is empty. `blocking_ringbuf_remove_all()` drains all currently available bytes into a caller-provided buffer, and `blocking_ringbuf_destroy()` reaps blocked waiters while freeing the owned storage.

Tested in `threads_blocking_ringbuf.c`

#### Barrier
Reusable barrier for a fixed set of threads. It uses a blocking lock plus two semaphores in a two-turnstile pattern so one generation cannot leak into the next. It must be reused by the same set of threads; other usage is undefined.

Tested in `threads_barrier.c`

#### Blocking Queue
FIFO queue of `GenericQueueElement`s with blocking remove. `blocking_queue_add()` appends one element and publishes one semaphore permit. `blocking_queue_remove()` blocks until a permit is available. `blocking_queue_try_remove()` and `blocking_queue_remove_all()` drain only the items that are currently available without blocking.

Tested in `threads_blocking_queue.c`

#### Condition Variable
Condition variable associated with exactly one predicate and one external blocking lock. `cond_var_wait()` publishes a private waiter semaphore before releasing the external lock, then re-acquires the external lock before returning. `cond_var_signal()` wakes one queued waiter, and `cond_var_broadcast()` wakes all queued waiters. Callers must hold the external lock for wait/signal/broadcast and must always re-check the predicate in a loop.

Tested in `threads_cond_var.c`

#### Gate
Latch-style event built from a blocking lock and condition variable. `gate_signal()` wakes all current waiters and leaves the gate open, so later calls to `gate_wait()` return immediately. `gate_reset()` closes the gate again so future waiters block until the next signal.

Tested in `threads_gate.c`

#### Event
Broadcast event built from a blocking lock, condition variable, and generation counter. `event_signal()` wakes all threads currently blocked in `event_wait()`, but the signal is not sticky. A waiter only returns after the generation advances past the value it observed when it started waiting.

Tested in `threads_event.c`

#### Shared Pointers
Reference-counted strong and weak pointers. The refcount control block is protected by a spinlock. Strong references keep the pointee alive, weak references keep only the control block alive, and weak-to-strong promotion succeeds only while the object still has at least one strong owner.

Tested in `threads_shared_ptr.c`, `threads_shared_ptr_parallel.c`, and `threads_shared_ptr_list.c`
