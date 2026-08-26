## Synchronization Primitives

The semaphore and rwlock implementations touch threading logic directly because they call `block()`. Everything else is layered on top of those primitives plus the atomic primitives.

### Destruction and lifetime

Synchronization-object destruction requires **external quiescence**. The
owner must first prevent new calls, release every holder, wake every blocked
operation through the primitive's normal wake path, and join or otherwise wait
for those operations to return. Only then may it call a `*_destroy()` or
`*_free()` function. The same rule applies transitively to composite objects
such as blocking locks, barriers, gates, events, queues, ring buffers, and
promises.

Queue quiescence also requires explicit payload ownership resolution. Spin,
generic spin, blocking, and bounded-buffer destroy operations reject a
nonempty queue with its address and size instead of clearing links and silently
abandoning TCBs or caller-owned elements. The owner must drain the queue and
free, recycle, or otherwise transfer every returned payload before destroy.

Destruction does not cancel, kill, or reap a blocked TCB. Cancellation return
values are not specified by the current kernel synchronization APIs, and
silently resuming an existing `void` acquisition as though it obtained a
permit would violate the primitive's ownership invariant. A future cancellable
API would need an explicit result propagated through every composed primitive.

Semaphore, rw-lock, and condition-variable operations publish an active-
operation count before their first CLH tail exchange. Blocking acquisitions
retain that count across the context switch and post-switch enqueue callback.
Destruction checks this state plus queued waiters and active holders before it
frees a CLH tail node. These checks diagnose operations that began before
destruction; the owner-level rule preventing new calls is still necessary
because no flag inside freed object storage can protect a caller that starts
after the storage's lifetime ends. All atomic counters rely only on the
documented sequentially-consistent memory model.

Composite queue teardown performs a non-mutating preflight of its payload queue
and every embedded semaphore before destroying the first lock. This matters for
an empty bounded buffer with a blocked consumer: its payload and permit counts
look like an ordinary empty buffer even though the remove semaphore still owns
a waiter. The preflight reports that operation while leaving both semaphores
intact. External quiescence remains required between preflight and destruction.

Every terminal TCB transition remains owned by `stop()`: it revokes the live
child TCB, publishes the exit result, releases the child descriptor's internal
reference, and only then queues the TCB for reaping. Synchronization teardown
must never bypass that sequence.

### Supported Sync Primitives

#### Disable/Restore Interrupts
Saves the current interrupt-mask register (IMR), clears it to disable interrupt
delivery, and later restores the exact saved IMR. `interrupts_disable()`
returns that prior register value; it does not inspect or change the TCB's
separate `can_preempt` state.

Exercised by `atomic_test.c`

#### Disable/Restore Preemption
Writes to the `can_preempt` field of the current thread's TCB, with interrupts briefly disabled so the update is atomic with respect to the PIT handler. `preemption_disable()` returns whether preemption was previously enabled, and `preemption_restore()` restores that saved state.

Tested in `threads_preempt_toggle.c`

#### Core Pinning
Writes to the `core_affinity` field of the current thread's TCB. `core_pin()` pins the thread to the current core, and `core_unpin()` restores to the old core affinity. The scheduler respects this field when choosing where a thread may run.

Tested in `threads_core_pin.c`

#### Spinlock
Uses atomic exchange until it acquires the lock. It disables interrupts before each attempt, and restores interrupts if an attempt fails. If the lock is acquired, interrupts remain disabled until the lock is released. For this reason, regions protected by a spinlock should be O(1).

Each lock records the exact owning TCB in addition to the TCB-local “owns a
spin lock” bit. Release must match both identities; holding one spin lock cannot
authorize release of another. CLH locks apply the same rule to their per-TCB
ticket handoff.

Tested in `atomic_test.c` and the expected-panic
`spin_lock_release_wrong_lock.c` and `clh_lock_release_wrong_lock.c` cases.

#### Preemption Spinlock
Same as spinlock, but disables preemption on each acquire attempt instead of interrupts. This is for cases where the critical section cannot be assumed to be O(1), but the caller still cannot block. The current use case is the print lock, so even idle threads can safely serialize debugging output.

Because this primitive is used before TCB bootstrap, ownership is the exact
core ID rather than a TCB pointer. Release of another core's or another lock
object is rejected. If a caller nests distinct preemption spin locks, releases
must be strict LIFO so each saved preemption state is restored at its matching
dynamic scope.

The expected-panic `preempt_spin_lock_release_wrong_lock.c` case exercises its
identity check; console tests exercise its contended production use.

#### Spin Barrier
Simple one-shot barrier for a known number of participants. Each thread decrements a shared counter and then spins until the counter reaches 0. It does not reset itself, so the caller must reinitialize the counter before reusing it.

Tested in `atomic_test.c`

#### Semaphore
Counting semaphore protected by a spinlock. `sem_down()` consumes a permit immediately if one is available; otherwise it blocks the current thread until a later `sem_up()`. `sem_up()` wakes exactly one waiter if any are queued, or increments the count if nobody is waiting. Kernel/composite code uses `sem_up()` as an invariant-enforcing operation and receives a diagnostic panic rather than signed overflow if it attempts to exceed `INT_MAX`; validation paths use `sem_try_up()` to report that condition without changing the count. Destruction requires the externally quiescent lifecycle above and rejects a live operation or waiter with an actionable kernel diagnostic.

Tested in `threads_semaphore.c`, `threads_semaphore_destroy_cleanup.c`, and the
expected-panic `semaphore_destroy_pre_enqueue.c` interleaving.

#### RwLock
Write-preferring reader-writer lock. Multiple readers may hold it at once, but only when there is no active writer and no queued writer. Writers acquire exclusive access. On release, a writer hands off to another waiting writer first; only when no writers are waiting are blocked readers released. Destruction requires no holder, waiter, acquire/release continuation, or new caller.

Tested in `threads_rw_lock.c`

#### Blocking Lock
Mutex-style lock implemented as a `Semaphore(1)`. Acquiring it may block, so unlike a spinlock it is suitable for longer critical sections. A successful acquire disables preemption and saves the caller's prior preemption state. Release wakes the next waiter and then restores that saved preemption state.

The lock records its exact owning TCB. A held flag alone is not sufficient:
another thread observing the lock as held cannot release it or restore the
owner's saved preemption state. Nested distinct blocking locks are permitted
for documented lock orders such as parent-inode before child-inode, but must be
released in strict LIFO order because each lock restores the preemption state
captured by its own acquisition.

Tested in `threads_cond_var.c`, `threads_barrier.c`, `threads_gate.c`,
`threads_event.c`, and the multicore expected-panic
`blocking_lock_release_nonowner.c` case.

#### Promise
One-shot publication primitive implemented with a semaphore. `promise_set()` stores a pointer and opens the promise. `promise_get()` blocks until the first set, then reposts the semaphore so every later getter also returns immediately with the same pointer.

Tested in `threads_promise.c`

#### Bounded Buffer
Fixed-capacity blocking queue of `GenericQueueElement`s. One semaphore counts free slots and another counts queued items. `bounded_buffer_add()` blocks while the buffer is full, and `bounded_buffer_remove()` blocks while it is empty. There are also non-blocking `remove_all()` and size helpers.

Destruction additionally requires the buffer to be empty; payload storage
remains caller-owned and is never silently released by the buffer. Both
semaphores are preflighted before either is destroyed, so a blocked producer or
consumer produces an actionable panic without partial teardown.

Tested in `threads_bounded_buffer.c` and the expected-panic
`bounded_buffer_destroy_waiter.c` interleaving.

#### Blocking RingBuf
Fixed-capacity blocking FIFO of `char` bytes backed by owned ring storage. A
blocking lock protects the indices plus producer/consumer-open predicates, and
condition variables wait for data or free space. `blocking_ringbuf_add()` and
`blocking_ringbuf_remove()` are invariant-enforcing wrappers for generic users.
The fallible forms additionally return when a side closes: producer close lets
queued bytes drain and then reports EOF, while consumer close wakes blocked
producers without committing another byte. Close broadcasts and predicate
checks occur under the same lock, so no wakeup can be lost under the
sequentially-consistent memory model. The API also provides a nonblocking size
snapshot but no bulk-drain operation. `blocking_ringbuf_destroy()` frees owned
storage only after the owner has closed the relevant sides and every producer,
consumer, close operation, and waiter continuation has returned.

Tested in `threads_blocking_ringbuf.c`

#### Barrier
Reusable barrier for a fixed set of threads. It uses a blocking lock plus two semaphores in a two-turnstile pattern so one generation cannot leak into the next. It must be reused by the same set of threads; other usage is undefined.

Tested in `threads_barrier.c`

#### Blocking Queue
FIFO queue of `GenericQueueElement`s with blocking remove. `blocking_queue_add()` appends one element and publishes one semaphore permit. `blocking_queue_remove()` blocks until a permit is available. `blocking_queue_try_remove()` and `blocking_queue_remove_all()` drain only the items that are currently available without blocking.

After external quiescence, the owner must use those removal operations to drain
and resolve every payload before destruction. Destroying a nonempty queue is an
invariant violation rather than an implicit discard operation.

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
