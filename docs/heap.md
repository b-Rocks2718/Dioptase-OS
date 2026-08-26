## Kernel Heap

The kernel heap provides `malloc()`, `free()`, and `leak()` for kernel-mode
code. It is an in-kernel allocator only; user programs use the separate CRT
heap in `root/crt/`.

The current kernel heap is built on top of `physmem`. Small allocations are
served from slab caches backed by 4 KiB physical frames. Larger allocations are
served by whole order-based `physmem` blocks.

### Arena / Geometry

The current heap does not allocate from a separate fixed heap arena. Heap-owned
slabs and large heap blocks come from the physical frame arena documented in
`physmem.md`:

- `FRAMES_ADDR_START = 0x100000`
- `FRAMES_ADDR_END = 0x7FB8000`
- `FRAME_SIZE = 4096`

Slab allocations use these object size classes:

- 4
- 8
- 16
- 32
- 64
- 128
- 256
- 512
- 1024

A small allocation request is rounded up to the first size class that can hold
the requested byte count. Requests larger than 1024 bytes bypass slabs and use
the large-allocation path.

### Supported Heap Features

#### Initialization

`heap_init()` must run after `physmem_init()` and before any heap allocation. It:

- initializes the large-allocation side table to `HEAP_LARGE_ALLOC_NONE`
- computes the object capacity for each slab size class
- initializes every global slab-cache list
- initializes every core-local heap free list

`heap_sync_init()` initializes the blocking locks for all slab caches and the
large-allocation side table. The boot path calls it only after early single-core
heap users have finished and before waking the other cores.

Before `heap_sync_init()`, heap locking is disabled and the heap must only be
used in the boot phase where no other core or thread can contend with it.

#### Slab Caches

Each slab is one 4 KiB physical page allocated from `physmem_alloc()`.

The slab page begins with `struct Slab` metadata. The allocator reserves enough
whole objects at the beginning of the page to hold that metadata, including the
debug allocation bitmap when `HEAP_DEBUG` is enabled. Allocatable objects begin
after the reserved metadata area and are linked through an in-object free list
while not allocated.

Each size class has one `SlabCache` containing three doubly linked slab lists:

- `partial_slabs`: slabs with at least one free object and at least one live object
- `full_slabs`: slabs with no free objects
- `empty_slabs`: slabs with no live objects

The cache keeps at most two empty slabs for reuse. When a third slab becomes
empty, it is returned to `physmem_free()`.

#### Per-Core Small-Object Free Lists

Small allocations normally go through a per-core free list before touching the
global slab cache.

`malloc()` for a slab size class:

- pins the current thread to its current core
- disables preemption while reading or updating that core's free list
- refills the per-core list from the global slab cache when the list is empty
- temporarily restores preemption while calling into the blocking slab-cache path
- returns one object from the per-core list

`free()` for a slab object:

- pins the current thread to its current core
- disables preemption while reading or updating that core's free list
- drains the per-core list back toward `PER_CORE_FREE_LIST_REFILL = 32` when it
  has reached `MAX_PER_CORE_FREE_LIST = 64`
- temporarily restores preemption while calling into the blocking slab-cache path
- pushes the freed object onto the per-core list

The per-core lists are safe only because the current thread remains pinned while
it may briefly re-enable preemption around blocking slab-cache calls.

#### Large Allocations

Requests larger than 1024 bytes allocate whole physical blocks with
`physmem_alloc_order()` or `physmem_leak_order()`.

The heap chooses the smallest order whose block size can hold the requested
byte count:

- order 0 gives one 4 KiB frame
- order `k` gives `2^k` contiguous frames
- the maximum order is `PHYS_FRAME_MAX_ORDER`

The returned pointer is the physical base address of the allocated block. The
caller owns at least the requested byte count; the backing block may be larger
because it is rounded up to a whole order.

Large allocations are tracked in a side table indexed by the first physical
frame of the block. `free()` uses that side table to route exact large-allocation
pointers back to `physmem_free_order()`.

If `free()` receives a frame-aligned pointer inside the physical-frame arena and
that frame is not the first frame of a live large heap allocation, it panics
instead of interpreting the page as a slab. This catches raw `physmem` pages,
large-allocation double frees, and other frame-base pointers before they can
corrupt slab metadata.

Current caller requirement: large allocations must be freed with the exact
pointer returned by `malloc()`. Interior pointers are invalid.

#### Lifetime Allocations

`leak(size)` is for objects intentionally kept until shutdown.

For slab-size allocations, `leak()` allocates from the same slab path as
`malloc()` but increments the heap leak counter instead of the malloc counter.
For larger allocations, `leak()` uses `physmem_leak_order()` so physmem leak
accounting also knows the block is intentional.

Current caller requirement: pointers returned by `leak()` must not be passed to
`free()`.

#### Teardown

`heap_destroy()` is called during kernel shutdown after all other heap users have
stopped.

The teardown path:

- disables heap locking before destroying heap-owned lock internals
- destroys slab-cache locks and the large-allocation lock if they were initialized
- returns all full, partial, and empty slab pages to `physmem`
- prints heap allocation/leak accounting

The heap does not walk per-core free lists during teardown; their objects live
inside slabs that are freed as whole pages.

### Locking / Blocking Semantics

After `heap_sync_init()`, each slab cache has one `BlockingLock`, and the
large-allocation side table has one `BlockingLock`.

Because `BlockingLock` may block:

- `malloc()`
- `leak()`
- `free()`

may all block.

Current caller requirement: heap allocation and free must not be used from
interrupt context, idle-thread scheduler paths, or any other context that cannot
sleep.

Small-object per-core list manipulation relies on:

- `core_pin()` / `core_unpin()` to keep the current thread on one core
- `preemption_disable()` / `preemption_restore()` while touching that core's
  free-list head and size fields

Global slab-cache operations may run with preemption restored while the thread
remains pinned to the current core.

### Debug Diagnostics

`HEAP_DEBUG` is controlled by the `HEAP_DEBUG` Makefile variable. When
`HEAP_DEBUG=yes`, the Makefile passes `-DHEAP_DEBUG=1` into BCC for kernel and
test C builds.

Debug mode adds:

- allocation counters for `malloc()`, `free()`, and `leak()`
- a per-slab allocation bitmap
- double-allocation detection
- double-free detection
- pointer range checks for slab frees
- object-size and object-alignment checks for slab frees
- poison writes to freed slab objects
- poison checks before a freed slab object is handed out again

The poison value is `HEAP_POISON = 0xABCDEFAA`.

The allocator reserves word 0 of a free object for the freelist `next` pointer,
so poison checking starts at word 1. Writes to word 0 after free are not detected
by the poison check.

Use-after-free writes are detected when the object is allocated again, not at the
moment of the invalid write.

The Makefile keeps a generated configuration stamp for kernel/test C assembly,
so changing `HEAP_DEBUG` rebuilds allocator code with the matching preprocessor
defines instead of reusing assembly generated for the previous mode.

### Current Limitations

#### No General-Purpose C Library Heap Contract

The kernel heap provides only `malloc()`, `free()`, and `leak()`. There is no
`calloc()`, `realloc()`, or explicit alignment API.

#### Zero-Size Allocation Is Not a Public Contract

The implementation asserts if `malloc(0)` or `leak(0)` reaches the common
allocation path. Callers must request a nonzero size.

#### Allocation Failure Panics

The backing `physmem` allocator panics on exhaustion. Heap callers should not
depend on `malloc()` returning `NULL` for out-of-memory conditions.

#### Large Allocations Have Less Debug Coverage

Large allocations are tracked only by their first frame and order. They do not
use slab allocation bitmaps or slab poison checks. Exact large-allocation
pointers are still validated against the large-allocation side table on `free()`.

#### Internal Fragmentation Is Expected

Slab allocations round up to a fixed size class. Large allocations round up to a
power-of-two number of whole frames.

### Tests

`make heap` runs the baseline-checked heap allocator tests, including panic-mode
negative tests.

The current heap tests include:

- `heap_test.c`
- `heap_threadsafe.c`
- `heap_double_allocation.c`
- `heap_double_free.c`
- `heap_interior_pointer_free.c`
- `heap_invalid_pointer_free.c`
- `heap_invalid_slab_free.c`
- `heap_use_after_free.c`
