# Kernel Design

## Initialization

Boot starts in `kernel/init.s`, core 0 initializes global subsystems, secondary
cores are woken with a boot IPI, and every core enters the idle-thread scheduler
loop after the start barrier.

See `kernel_init.md` for more details.

## Threading

Structure:
- Allocates a fixed size stack per thread
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
- blocking ringbuf
- blocking queue
- rwlock
- barrier
- gate (implements wait(), signal(), and reset(); signal unblocks waiters, and calls to wait() after signal will not block)
- event (like gate, except it does not remain open after call to signal(). Therefore does not need a reset() method)
- shared pointers

See `sync.md` for more details.

## Heap
Global heap shared by all cores. Small allocations use slab caches with
per-core free lists, and larger allocations use whole order-based `physmem`
blocks.

See `heap.md` for more details.

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
Uses a 2-level table, similar to x86

TLB is software managed, so any miss invoked the tlb handler

VMEM currently supports:
- private anonymous
- private file-backed
- shared file-backed

Shared anonymous is not yet supported

See `vmem.md` for more details

## Syscalls

User programs enter the kernel with the single `trap` instruction.

- `r1`: trap code
- `r2-r8`: trap-specific arguments
- IVT entry `0x004`: shared trap vector

Current trap code assignments are documented in `syscalls.md`.

## Terminal Foreground Control

Dioptase-OS currently has one foreground child slot for
the interactive terminal.

- The shell sets the foreground child to the external command it is about to
  wait for.
- The terminal sends Ctrl-C to that foreground child with
  `signal_foreground(DIOPTASE_SIGNAL_TERMINATE)`.
- Normal keyboard bytes still flow through the terminal input pipe inherited as
  `STDIN`.
- A foreground child's descriptor records whether that live child used a direct
  VGA configuration or mapping trap. The descriptor state lock protects this
  claim together with the child TCB lifetime.
- After `wait_child()`, the shell clears the foreground slot.
  `set_foreground_child(-1)` atomically returns the old descriptor's display
  claim while the foreground and descriptor locks exclude concurrent claims.
- If the returned claim is set, the shell queues the terminal-private display
  recovery sequence before it prints the next prompt. The terminal, rather than
  the shell, resets VGA and renderer state in pipe order.
