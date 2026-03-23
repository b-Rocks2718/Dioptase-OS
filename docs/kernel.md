# Kernel Design

## Threading

Structure:
- Allocates a fixed size stack per thread (TODO: use page allocator instead of heap, and use guard pages to detect overflow)
- Per-core and global ready queues
- round-robin scheduling
- Preemptive, timer isr context switches to idle threads, idle thread cannot be preempted and finds next ready thread to switch to
- TODO: better scheduling

Supported Sync Primatives:
- enable/disable interrupts
- enable/disable preemption
- TODO: core pinning
- spinlock
- blocking lock
- semaphore
- promise
- bounded buffer
- blocking queue
- rwlock
- shared pointers
- barrier
- gate (implements wait(), signal(), and reset(); signal unblocks waiters, and calls to wait() after signal will not block)
- event (like gate, except it does not remain open after call to signal(). Therefore does not need a reset() method)

## Heap
Global heap shared by all cores, free blocks kept in doubly linked list  
TODO: slab allocator, buddy allocator, per-core freelists/partial slabs

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

## Virtual Memory
TLB is software managed

TODO: all of VM and page cache

## Syscalls

TODO
