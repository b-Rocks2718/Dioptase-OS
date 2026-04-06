## Physmem Allocator

The kernel physical page allocator manages the frame arena documented in
`kernel_mem_map.md` with a buddy allocator plus a small per-core order-0 cache.
It is an in-kernel allocator only; there is no user-visible virtual memory
layer yet.

### Arena / Geometry

`physmem` manages the physical address range `0x800000 - 0x7FBCFFF`, which is
`PHYS_FRAME_COUNT = 30653` frames of size `FRAME_SIZE = 4096` bytes.

Buddy numbering is defined in **frame-index space relative to
`FRAMES_ADDR_START`**, not in absolute physical address space.

- frame index 0 corresponds to physical address `0x800000`
- frame index `n` corresponds to `FRAMES_ADDR_START + n * FRAME_SIZE`
- the buddy of block index `i` at order `k` is `i ^ (1 << k)`

This matters because the arena does not begin at physical address 0 and the
allocator supports orders up to `PHYS_FRAME_MAX_ORDER = 14`.

Because `PHYS_FRAME_COUNT` is not a power of two, initialization does not build
one single top-level buddy tree. Instead, `physmem_init()` decomposes the arena
into the largest aligned power-of-two blocks that fit and seeds one free list
per order with that forest of top-level blocks.

### Supported Physmem Features

#### Initialization

`physmem_init()` must run once during early boot before concurrent allocator
use. It:

- validates that the documented arena size matches `PHYS_FRAME_COUNT`
- initializes the global buddy lock
- seeds the buddy free lists across the whole frame arena
- initializes every core-local order-0 cache

The allocator assumes the frame arena is already reserved exclusively for
physical-page allocation and is not shared with the heap, kernel image, or MMIO.

#### Buddy Free Lists

There is one global free list per order `0..PHYS_FRAME_MAX_ORDER`.

Each free block stores its list metadata in the first words of the free block
itself:

- `prev`
- `next`
- `free_order`

This metadata is valid only while the block is free. Once allocated, the caller
owns the whole block contents.

The allocator also keeps a bitmap indexed by frame index. A set bit means
"this frame index is currently the head of a free block." It is **not** a full
per-page allocation bitmap for every page in a larger free block.

#### Order-Based Allocation

`physmem_alloc_order(order)` allocates one block of order `order`.

- order 0 returns one 4 KiB page
- order `k` returns `2^k` contiguous pages
- the returned block is aligned to its size in frame-index space

The allocator searches for the smallest non-empty free list at or above the
requested order. If it finds a larger block, it repeatedly splits that block,
keeps the left half, and returns the right half of each split to the next lower
order free list until the requested order is reached.

Allocation currently panics on exhaustion instead of returning `NULL`.

#### Order-Based Free / Coalescing

`physmem_free_order(page, order)` frees one previously allocated block and tries
to coalesce it upward.

The free path:

- validates that the address is inside the documented frame arena
- validates frame alignment and alignment to the supplied order
- computes the buddy in frame-index space
- coalesces only when the buddy is:
  - still inside the frame arena
  - marked free in the free-head bitmap
  - recorded as the same order

When coalescing succeeds, the combined block keeps the lower frame index and
the order increases by one. Coalescing stops at the first missing, out-of-range,
or differently sized buddy.

#### Order-0 Fast Path

`physmem_alloc()` and `physmem_free()` are optimized for single-page traffic.

Each core has a local cache of `LOCAL_CACHE_SIZE = 16` order-0 pages.

`physmem_alloc()`:

- pins the current thread to the current core
- acquires that core's cache lock
- refills the local cache from the global buddy allocator if empty
- pops and returns one cached page

`physmem_free()`:

- pins the current thread to the current core
- acquires that core's cache lock
- pushes the page into the local cache if there is room
- otherwise returns the page directly to the global buddy allocator

Higher-order allocations and frees bypass the per-core caches and always use the
global buddy allocator directly.

### Locking / Blocking Semantics

The global buddy allocator is protected by one `BlockingLock`. Each core-local
order-0 cache also has its own `BlockingLock`.

Because `BlockingLock` may block:

- `physmem_alloc_order()`
- `physmem_free_order()`
- `physmem_alloc()`
- `physmem_free()`

may all block.

Current caller requirement: these functions must not be used from interrupt
context or any other context that cannot sleep.

`physmem_alloc()` and `physmem_free()` also rely on `core_pin()` /
`core_unpin()` while touching per-core state, because `get_per_core()` requires
interrupts/preemption disabled or an explicitly pinned thread.

### Current Limitations

#### No Explicit Double-Free Detection

The allocator asserts on invalid address, range, and alignment mistakes, but it
does not currently keep a separate "allocated vs already freed" state that would
turn every double-free into a clean diagnostic.

#### Caller-Supplied Free Order Is Trusted

`physmem_free_order(page, order)` validates that the address is aligned for the
supplied order, but it does not independently remember the allocation order of
live blocks. Supplying the wrong order is a caller bug.

#### No Global Reclaim From Other Cores' Local Caches

Order-0 pages sitting in another core's local cache are temporarily invisible to
the global buddy free lists. The current implementation does not have a global
"flush every core-local cache and retry" path on allocation failure.

This is why backend-only tests explicitly drain per-core caches before making
global buddy allocator assertions.

#### Order-0 Caches Only

Only order-0 pages are cached per core. Larger blocks always go through the
global buddy lock.

### Diagnostics

Allocator assertions and panic messages are intended to catch:

- out-of-range frame addresses
- misaligned block addresses
- invalid orders
- arena geometry mismatches at boot
- out-of-memory conditions during allocation

### Tests

`physmem` is currently exercised by:
- `physmem_test.c`

`physmem_test.c` currently checks:

- concurrent order-0 churn across multiple worker threads
- no duplicate live-page handout
- page-content retention while pages are live
- higher-order alignment and writability for every supported order
- a large direct order-0 backend sample after draining per-core caches
