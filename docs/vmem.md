## Virtual Memory

The kernel virtual memory layer gives each thread its own page directory and a
sorted list of virtual memory entries (VMEs). VMEs describe reserved virtual
address ranges; physical pages and page tables are allocated lazily on TLB
miss.

### Address Space Structure

The current implementation uses:

- `FRAME_SIZE = 4096` byte pages
- one 1024-entry page directory per thread
- one 1024-entry page table for each populated directory slot

The address space is roughly split in half:
- `KERNEL_VMEM_START = 0x10000000` - `KERNEL_VMEM_END = 0x7FFFFFFF` is used by the kernel
- `USER_VMEM_START = 0x80000000` - `USER_VMEM_END = 0xFFFFFFFF` is reserved for user programs

Virtual address translation uses the standard 10 / 10 / 12 split:

- bits `31..22`: page directory index
- bits `21..12`: page table index
- bits `11..0`: page offset

Each thread gets a fresh, empty page directory in `thread_()`. The `TCB.pid`
field stores the physical address of that page directory, and the same value is
loaded into the hardware PID register when that thread's address space becomes
active.

Kernel thread creation does not inherit an address space: `thread_()` gives the
new thread an empty page directory and `vme_list`. Process creation through
`fork()` is different; `vmem_fork()` clones the parent's VME metadata, resident
user mappings, and resident direct mappings into a new child address space.
Private RAM is snapshotted, while direct physical/MMIO VMEs remain borrowed
aliases of the same live physical pages in both processes.

### PDE / PTE Format

Page-directory entries and page-table entries use the same software-visible
layout:

`Bits 31..12 = aligned physical address | Bits 11..0 = flags`

The currently defined permission and state bits are:

- `VMEM_READ  = 0x01`
- `VMEM_WRITE = 0x02`
- `VMEM_EXEC  = 0x04`
- `VMEM_USER  = 0x08`
- `VMEM_GLOBAL = 0x10`
- `VMEM_VALID = 0x20`
- `VMEM_DIRTY = 0x40`

Current VM code uses `READ`, `WRITE`, `EXEC`, and `VALID` when constructing
PTEs for mapped pages. `VMEM_USER`, `VMEM_GLOBAL`, and `VMEM_DIRTY` are defined
but are not part of the current `mmap()` fault path.

### TLB

The TLB has 16 entries.

Each entry is keyed by:

- `PID` (32 bits)
- `VPN` (20 bits)

and stores:

- `PPN` (15 bits)
- flags

The hardware-managed valid bit is separate from the software-visible flags.
Current software uses the low 5 flag bits in the TLB:

- `G`: global
- `U`: user
- `X`: executable
- `W`: writable
- `R`: readable

`tlbr rA, rB` looks up the current `PID` plus `(rB & 0xFFFFF000)` and returns
the matching TLB value in `rA`.

`tlbw rA, rB` writes a TLB entry using the current `PID` plus
`(rB & 0xFFFFF000)` as the key and `(rA & 0x7FFFFFF)` as the value.

`vmem_core_init()` flushes the local core's TLB and clears the active PID to 0
at boot.

### Supported VM Features

#### Global Initialization

`vmem_global_init()` must run once during boot before normal VM use. It:

- registers the TLB miss handler
- initializes the global file-page cache used by file-backed mappings

#### Page Directory / Page Table Allocation

`create_page_directory()` and `create_page_table()` allocate one physical page
from `physmem` and zero all 1024 entries.

Page tables are allocated lazily. A page directory slot remains invalid until
the first fault reaches a virtual address in that 4 MiB region.

#### VME List Management

Each thread owns a sorted, non-overlapping singly linked list of `struct VME`.
Each VME records:

- `start` and `end` virtual addresses
- the original requested `size`
- `MMAP_*` flags
- optional file backing (`struct Node*` plus page-aligned `file_offset`)
- an explicit direct-physical-mapping marker and physical base address

`mmap(size, file, file_offset, flags)`:

- rounds `size` up to a whole number of pages for address-space reservation
- keeps the original `size` in the VME for last-page file-backed accounting
- finds the first gap large enough to hold the rounded mapping, starting at
  `KERNEL_VMEM_START` for kernel mappings or `USER_VMEM_START` when
  `MMAP_USER` is set
- inserts the new VME into the calling thread's sorted list
- clones the passed `Node` wrapper for file-backed mappings, so the caller may
  later free its own wrapper independently
- returns `NULL` if size cannot be page-rounded representably or first-fit
  cannot find a sufficiently large range

Current caller requirements:

- `size` must be nonzero
- file-backed mappings must use a `file_offset` that is a multiple of
  `FRAME_SIZE`
- `MMAP_USER` selects the user half of the address space; otherwise the
  allocation is searched in the kernel half
- mappings are placed only by the kernel's first-fit policy; there is no
  fixed-address hint API
- the calling context must tolerate heap / VM allocator work; this is not an
  interrupt-context API

`mmap_at(size, file, file_offset, flags, vaddr)`:

- reserves the exact page-aligned range beginning at `vaddr`
- requires `vaddr` and `flags` to name the same address-space half
- panics if the requested range overlaps an existing VME

`mmap_stack(size, flags)`:

- reserves an anonymous stack in the user half using a top-down search
- requires `MMAP_USER`
- returns the stack's lowest virtual address; callers derive the initial stack
  pointer from the top of the reserved range
- currently uses the highest representable page-aligned exclusive end below
  `USER_VMEM_END` because `struct VME` stores an exclusive 32-bit end address

`munmap(p)`:

- requires `p` to equal the exact start address returned by `mmap()`
- removes the entire VME; there is no partial unmap or VME splitting
- frees or releases any resident backing pages for that VME
- invalidates the local core's TLB entries covering that range

Passing an address that is not the start of a live VME is a kernel bug and
causes a panic.

#### Private Anonymous Mappings

A mapping is private anonymous when:

- `file == NULL`
- `MMAP_SHARED` is not set

On the first kernel TLB miss for a page in that VME, the kernel:

- allocates one physical page
- zero-fills the page
- installs a PTE using the requested `READ`, `WRITE`, and `EXEC` permissions

Each VME owns its own anonymous pages. Writes stay private to that mapping.
`munmap()` and thread teardown free those pages directly back to `physmem`.

#### Private File-Backed Mappings

A mapping is private file-backed when:

- `file != NULL`
- `MMAP_SHARED` is not set

On the first fault for each page, the kernel:

- acquires the corresponding file page from the global page cache
- allocates one private physical page
- copies the cached page contents into that private page
- releases the page-cache reference immediately
- installs the private page into the faulting thread's page tables

This is currently an eager private copy on first fault. There is no copy-on-
write mechanism.

The file page is zero-filled past end-of-file when loaded into the page cache.
Writes through a private mapping do not update the backing file.

#### Shared File-Backed Mappings

A mapping is shared file-backed when:

- `file != NULL`
- `MMAP_SHARED` is set

On the first fault for each page, the kernel:

- acquires the corresponding page from the global page cache
- uses the cache page directly as the mapped physical page
- for a writable mapping, conservatively marks the page dirty and max-merges
  the number of bytes that this mapping exposes in that page
- installs a PTE pointing at that shared page

All mappings of the same file page share one in-memory cache page, keyed by the
inode plus the page-aligned byte `file_offset` used for that page. Merely
acquiring the page does not change its writeback extent. Consequently a wider
read-only or private mapping cannot enlarge an already-dirty writable mapping's
eventual file write, while a later wider writable fault can do so.

The ISA does not currently provide a usable dirty transition for this path, so
the first writable fault conservatively authorizes writeback of the mapping's
complete logical extent in that page even if user code does not later modify
every byte. On the last `page_cache_release()` for a dirty cached page, the
kernel writes that max-merged extent to the backing file and frees the cached
physical page. Clean pages are freed without writeback.

`vmem_truncate_file()` serializes shrink-only truncate with cache acquisition,
dirty publication, and final release. It holds the global page-cache lock while
`node_shrink()` holds the inode lock, then caps a straddling dirty page to the
new EOF and clears dirty state for pages wholly beyond it. An old dirty release
therefore cannot undo truncate. A writable page fault ordered after truncate
may deliberately publish a wider extent and extend the file again, preserving
the existing shared-mapping extension semantics. An already-resident writable
PTE does not generate another software dirty event after truncate; extending
again requires a later fault that republishes an extent.

This gives shared visibility between concurrent mappings of the same cached file
page. The current implementation defines sharing in terms of the page cache; it
does not define coherence with `node_read_all()` / `node_write_all()` or syscall
file I/O that bypasses that cache. In particular, direct file writes can leave a
live cached page stale or later be overwritten by dirty cached writeback; that
separate coherence problem remains unresolved.

#### Shared Anonymous Mappings

`MMAP_SHARED` with `file == NULL` is reserved but not implemented yet.

Current behavior:

- faulting such a VME panics in `tlb_miss_handler()`
- unmapping such a VME asserts in `munmap()` / `unmap_vme()`

#### Direct Physical/MMIO Mappings

`mmap_physmem(size, paddr, flags)` is a kernel-only API. It creates a VME whose
virtual pages refer directly to one contiguous physical window; it does not
allocate, copy, or take ownership of the backing pages. Its caller contract is:

- `size` is nonzero and page-roundable without 32-bit unsigned overflow
- `paddr` is 4096-byte aligned
- the page-rounded window is wholly inside the 27-bit physical address space
  `0x0000000..0x7FFFFFF` defined by the ISA and `../../docs/mem_map.md`
- only `MMAP_READ`, `MMAP_WRITE`, `MMAP_EXEC`, and `MMAP_USER` are accepted;
  `MMAP_SHARED` describes page-cache ownership and is invalid here because a
  direct mapping is already a live physical alias

Malformed requests are kernel bugs and panic with the physical address, size,
or flags in the diagnostic. Failure to find a virtual range is ordinary
resource exhaustion and returns `NULL`.

An exact repeated request in the same TCB—same physical base, requested size,
and flags—returns the existing virtual base instead of adding another VME.
Because translation is page-granular, the VME reserves and maps the rounded
page extent. A device client must access only byte ranges whose behavior is
defined by the physical memory map; behavior of undocumented trailing bytes is
unspecified.

Direct physical pages are borrowed for the complete VME lifetime. `munmap()`
and address-space teardown invalidate translations and free page tables, but
never pass those physical pages to the `physmem` allocator.

### Fault Handling

The ISA provides TLB-miss vector at `0x82` / `0x208`, and the kernel
registers it to `tlb_miss_handler()`.

For a tlb miss, it:

- finds the containing VME in the current thread's `vme_list`
- allocates a page table if the enclosing PDE is still invalid
- allocates or acquires the required backing page depending on the VME type
- installs a PTE with the requested permissions
- writes the resolved translation into the TLB

If no containing VME exists, the kernel panics.

### Address-Space Cloning During `fork()`

`fork()` calls `vmem_fork()` before making the child runnable. The clone path:

- copies the parent's VME metadata into a separately owned child VME list
- allocates a new page directory and page tables for the child
- leaves nonresident pages nonresident so either process can fault them in later
- copies each resident direct physical/MMIO PTE verbatim, so pages faulted
  before fork and pages first faulted afterward alias the same live physical
  page in the parent and child
- acquires another page-cache reference for each resident shared file-backed
  page and maps the same cache page in both processes
- allocates and copies a new physical page for each resident private user page

The parent and child therefore have independent page tables and private mapping
contents. Shared file-backed mappings continue to refer to the same page-cache
entries, and direct physical mappings continue to refer to the same device or
physical pages. Private pages are currently copied eagerly during `fork()`;
copy-on-write cloning is not implemented.

### Address-Space Teardown

Thread teardown calls `vmem_destroy_address_space()` and then frees the VME
list metadata.

Address-space teardown:

- walks every VME and unmaps any resident pages
- frees empty page tables
- frees the page directory itself
- drops any cloned file wrappers stored in VMEs

For private mappings, teardown frees resident physical pages directly. For
shared file-backed mappings, teardown releases the page-cache references instead
of freeing the shared pages directly. For direct physical mappings, teardown
only removes the borrowed translations.

### Concurrency / Invariants

Current VM code assumes:

- each thread mutates only its own `vme_list`
- each VME list remains sorted and non-overlapping
- one address space is active on only one core at a time

That last point matters because `munmap()` invalidates TLB entries only on the
current core. There is no cross-core TLB shootdown mechanism yet.

Shared file-backed page sharing is implemented with the global page cache, which
has its own lock and reference counts. Cache fill, dirty publication, final
writeback, and serialized truncate are sequentially consistent under that
lock. Any operation needing both locks acquires `page_cache.lock` first and the
cached inode lock second. No ext path may acquire the page-cache lock while it
already holds an inode lock; preserving that one-way order prevents an inverse
lock dependency across blocking storage operations.

### Current Limitations

#### Light User Address-Space Coverage

User-mode TLB misses currently reuse the same VM fault path as kernel-mode
misses. Current tests and current `mmap()` usage are still mostly kernel-side,
so user VM remains lightly exercised.

#### No Shared Anonymous Support

The API flag combination exists, but the fault and unmap paths still reject it.

#### No Shared Address Spaces Between Threads

Kernel thread creation still allocates a fresh page directory and starts with an
empty VME list. `fork()` clones a snapshot into a distinct child address space;
there is no thread-creation operation that makes multiple threads concurrently
use the same page tables or `vme_list`.

#### No Partial `munmap()`

`munmap()` can remove only an entire VME by its exact base address. It cannot
trim, split, or punch holes inside an existing mapping.

#### No Copy-On-Write

Private file-backed mappings eagerly copy the cached file page on first fault.
In addition, `fork()` eagerly copies every resident private user page into a new
child frame. The kernel does not yet share either kind of clean private page and
break sharing later on write.

#### Local-Core-Only TLB Invalidation

Unmap invalidates the current core's TLB only. If future work introduces
simultaneously active shared address spaces, this will need real TLB shootdown.

#### Page-Aligned File Offsets Only

File-backed mappings currently require `file_offset % FRAME_SIZE == 0`.

The implementation and tests assume file-backed sharing happens at whole-page
granularity, so non-page-aligned file offsets are invalid rather than a
supported corner case.

### Diagnostics

The VM subsystem currently asserts or panics on:

- malformed VME list ordering or overlap
- invalid `munmap()` addresses
- TLB misses that do not fall inside any VME
- attempts to fault or unmap shared anonymous mappings
- malformed kernel-only `mmap_physmem()` size, physical range, alignment, or
  flags

These failures are treated as kernel bugs, not recoverable runtime conditions.
Plain `mmap()` and `mmap_physmem()` virtual-range exhaustion are instead normal
fallible results; public callers translate `NULL` to the syscall's `-1` result.

### Tests

VM functionality is currently exercised by:

- `vmem_simple.c`
- `vmem_private_anonymous.c`
- `vmem_private_file.c`
- `vmem_shared_file.c`
- `user_physmem_alias.dir/sbin/init.c`

Those tests currently cover:

- private anonymous allocation, zero-fill, and unmap
- concurrent private anonymous mapping churn and hole reuse
- private file-backed mapping isolation from the backing file
- concurrent private file-backed mappings of the same file
- shared file-backed visibility across concurrent mappings
- shared file-backed persistence back to disk after unmap
- dirty writeback-extent merging across differently sized writable aliases
- non-extension by a wider read-only alias
- dirty partial-page and wholly-beyond-EOF behavior across truncate, plus a
  later writable fault extending the file again
- page-aligned nonzero `file_offset` for both private and shared file-backed
  mappings
- page-by-page physical-window translation, idempotent display getters, and
  live parent/child aliasing of resident direct mappings across `fork()`

They do not currently cover:

- shared anonymous mappings
- non-page-aligned `file_offset`
- multi-core TLB invalidation for a shared address space
