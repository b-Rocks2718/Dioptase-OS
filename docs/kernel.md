# Kernel Design

## Threading

Structure:
- Allocates a fixed size stack per thread (TODO: use page allocator instead of heap, and use guard pages to detect overflow)
- Per-core and global ready queues with load-balancing
- Kernel can set threads as `HIGH_PRIORITY`, `NORMAL_PRIORITY`, and `LOW_PRIORITY`. Within each priority, MLFQ is used to schedule threads
- Preemptive, timer isr context switches to idle threads, idle thread cannot be preempted and finds next ready thread to switch to

See `threading.md` and `scheduling.md` for more details.

Supported Sync Primatives:
- enable/disable interrupts
- enable/disable preemption
- core pinning
- spinlock (disables interrupts on acquire, restores on release)
- preemption spinlock (disables preemption on acquire, restores on release)
- blocking lock
- semaphore
- promise
- bounded buffer
- blocking queue
- rwlock
- barrier
- gate (implements wait(), signal(), and reset(); signal unblocks waiters, and calls to wait() after signal will not block)
- event (like gate, except it does not remain open after call to signal(). Therefore does not need a reset() method)
- shared pointers

See `sync.md` for more details.

## Heap
Global heap shared by all cores, free blocks kept in doubly linked list  
TODO: replace with slab allocator

## File System
ext2 rev 0

Supported:
- Read files, directories, and symlinks
- create new files, directories, and symlinks
- write to files
- rename files/directories/symlinks
- delete files and symlinks, and delete empty directories

Not yet supported:
- rwx permissions
- uid/gid

See `filesystem.md` for more details.

## Virtual Memory
TLB is software managed

TODO: all of VM and page cache

## Syscalls

TODO
