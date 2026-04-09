## Virtual Memory

The kernel virtual memory layer gives each thread its own page directory and a
sorted list of virtual memory entries (VMEs). VMEs describe reserved virtual
address ranges; physical pages and page tables are allocated lazily on TLB
miss.

Current usage is kernel-side `mmap()` for kernel threads. The user-mode miss
path is not implemented yet.

### Address Space Structure

The current implementation uses:

- `FRAME_SIZE = 4096` byte pages
- one 1024-entry page directory per thread
- one 1024-entry page table for each populated directory slot
- `VMEM_START = 0x80000000` as the base of dynamically allocated virtual memory

Virtual address translation uses the standard 10 / 10 / 12 split:

- bits `31..22`: page directory index
- bits `21..12`: page table index
- bits `11..0`: page offset

Each thread gets a fresh, empty page directory in `thread_()`. The `TCB.pid`
field stores the physical address of that page directory, and the same value is
loaded into the hardware PID register when that thread's address space becomes
active.

There is currently no address-space inheritance or cloning during thread
creation. A newly created thread starts with an empty `vme_list`.

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

- registers the kernel and user TLB miss handlers
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

`mmap(size, file, file_offset, flags)`:

- rounds `size` up to a whole number of pages for address-space reservation
- keeps the original `size` in the VME for last-page file-backed accounting
- finds the first gap large enough to hold the rounded mapping, starting at
  `VMEM_START`
- inserts the new VME into the calling thread's sorted list
- clones the passed `Node` wrapper for file-backed mappings, so the caller may
  later free its own wrapper independently

Current caller requirements:

- `size` must be nonzero
- file-backed mappings must use a `file_offset` that is a multiple of
  `FRAME_SIZE`
- mappings are placed only by the kernel's first-fit policy; there is no
  fixed-address hint API
- the calling context must tolerate heap / VM allocator work; this is not an
  interrupt-context API

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
- installs a PTE pointing at that shared page

All mappings of the same file page share one in-memory cache page, keyed by the
inode plus the page-aligned byte `file_offset` used for that page.

On the last `page_cache_release()` for that cached page, the kernel writes back
`file_bytes` bytes to the backing file and frees the cached physical page.

This gives shared visibility between concurrent mappings of the same cached file
page. The current implementation defines sharing in terms of the page cache; it
does not separately define coherence with any file-write path that bypasses that
cache.

#### Shared Anonymous Mappings

`MMAP_SHARED` with `file == NULL` is reserved but not implemented yet.

Current behavior:

- faulting such a VME panics in `tlb_kmiss_handler()`
- unmapping such a VME asserts in `munmap()` / `unmap_vme()`

### Fault Handling

`tlb_kmiss_handler()` is the active VM fault handler today.

For a kernel miss, it:

- finds the containing VME in the current thread's `vme_list`
- allocates a page table if the enclosing PDE is still invalid
- allocates or acquires the required backing page depending on the VME type
- installs a PTE with the requested permissions
- writes the resolved translation into the TLB

If no containing VME exists, the kernel panics.

`tlb_umiss_handler()` is not implemented and currently panics unconditionally.

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
of freeing the shared pages directly.

### Concurrency / Invariants

Current VM code assumes:

- each thread mutates only its own `vme_list`
- each VME list remains sorted and non-overlapping
- one address space is active on only one core at a time

That last point matters because `munmap()` invalidates TLB entries only on the
current core. There is no cross-core TLB shootdown mechanism yet.

Shared file-backed page sharing is implemented with the global page cache, which
has its own lock and reference counts.

### Current Limitations

#### No User Address-Space Fault Handling

User-mode TLB misses panic. Current tests and current `mmap()` usage are kernel-
side only.

#### No Shared Anonymous Support

The API flag combination exists, but the fault and unmap paths still reject it.

#### No Address-Space Inheritance

Thread creation always allocates a fresh page directory and starts with an empty
VME list. There is no `fork()`-style address-space clone and no way to make a
new thread automatically observe an existing anonymous mapping.

#### No Partial `munmap()`

`munmap()` can remove only an entire VME by its exact base address. It cannot
trim, split, or punch holes inside an existing mapping.

#### No Copy-On-Write

Private file-backed mappings eagerly copy the cached file page on first fault.
The kernel does not yet share clean private pages and break sharing later on
write.

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
- kernel TLB misses that do not fall inside any VME
- attempts to fault or unmap shared anonymous mappings
- any user-mode TLB miss

These failures are treated as kernel bugs, not recoverable runtime conditions.

### Tests

VM functionality is currently exercised by:

- `vmem_simple.c`
- `vmem_private_anonymous.c`
- `vmem_private_file.c`
- `vmem_shared_file.c`

Those tests currently cover:

- private anonymous allocation, zero-fill, and unmap
- concurrent private anonymous mapping churn and hole reuse
- private file-backed mapping isolation from the backing file
- concurrent private file-backed mappings of the same file
- shared file-backed visibility across concurrent mappings
- shared file-backed persistence back to disk after unmap
- page-aligned nonzero `file_offset` for both private and shared file-backed
  mappings

They do not currently cover:

- shared anonymous mappings
- user-mode virtual memory
- non-page-aligned `file_offset`
- multi-core TLB invalidation for a shared address space
