# Kernel Design

## Threading

Structure:
- Allocates a fixed size stack per thread
- Global ready queue
- round-robin scheduling
- Preemptive, timer isr context switches to idle threads, idle thread cannot be preempted and finds next ready thread to switch to
- TODO: switch to per-core queues
- TODO: core pinning
- TODO: better scheduling

Supported Sync Primatives:
- enable/disable interrupts
- enable/disable preemption
- spinlock
- blocking lock
- semaphore
- promise
- bounded buffer
- blocking queue
- rwlock
- shared pointers
- barrier (TODO: make reusable)
- TODO: one-sided barrier

## File System
ext2 rev 0

## Virtual Memory
tlb is software managed

