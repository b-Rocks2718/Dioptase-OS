## Kernel Initialization

This document describes the current boot path from BIOS handoff to the first
user program. It is about the kernel-side sequencing in `kernel/init.s`,
`kernel/kernel_entry.c`, `kernel/threads.c`, and `kernel/kernel_main.c`.

Related documents:

- `BIOS.md` for the boot-sector kernel loading contract
- `kernel_mem_map.md` for fixed kernel load addresses and boot stacks
- `../../docs/ISA.md` for `PSR`, `IMR`, IVT, `IPI`, `eoi`, and `rfe`
- `../../docs/abi.md` for the function-call ABI used when assembly calls C
- `physmem.md`, `heap.md`, `vmem.md`, `devices.md`, `threading.md`,
  `scheduling.md`, and `syscalls.md` for the initialized subsystems

### Entry State

The BIOS loads the kernel sections described in `BIOS.md` and jumps to the
kernel text load address. The kernel entry symbol is `_start` in
`kernel/init.s`.

`_start` performs the per-core machine setup needed before C code can run:

- sets `IMR` to `0`, disabling all interrupts
- executes `eoi all` to clear any pending interrupt state
- sets `PSR` to `1`, so execution is in kernel mode
- selects a fixed early kernel stack for the current core using `cid`
- calls `kernel_entry()`

If `kernel_entry()` ever returns, `_start` halts the core.

The early C boot path assumes interrupts are disabled. This is required because
per-core thread state and interrupt handlers are not initialized yet.

### Core 0 Global Initialization

Only core 0 performs one-time global initialization. Other cores do not run the
global subsystem constructors.

Core 0 initializes subsystems in this order:

- `vga_init()` prepares text-mode output when VGA output is enabled.
- `register_spurious_handlers()` installs a halt-on-use handler for unused IVT
  entries. It intentionally leaves the IPI vector alone so boot IPIs can be
  installed separately.
- `physmem_init()` initializes the physical frame allocator.
- `heap_init()` initializes the heap while heap locking is still disabled.
- The boot path allocates one idle-thread CLH node for each possible core.
- `vmem_global_init()` installs the TLB miss handler and initializes the page
  cache.
- `pit_init(3000)` registers the PIT handler and programs the timer device.
  Interrupts are still globally disabled at this point.
- `bootstrap()` creates core 0's idle-thread TCB context.
- `threads_init()` initializes scheduler queues and creates the reaper thread
  with `setup_thread()`.
- `uart_init()`, `audio_init()`, `exc_init()`, `sd_init()`, and `ps2_init()`
  register device and exception handlers and initialize device-side state.
- `ext2_init(&fs)` reads the ext2 filesystem metadata from SD drive 1 and opens
  the root inode.
- `trap_init()` installs the shared syscall/trap handler.

During this phase, `bootstrapping` is still true. Kernel daemon threads created
with `setup_thread()` do not end bootstrapping and do not count as active user
work. SD waits are allowed to busy-wait during this phase because normal thread
blocking and interrupt-driven wakeups are not fully live yet.

### First Runnable Thread

After the global subsystems are ready, core 0 creates the `kernel_main` thread:

- allocates a `struct Fun`
- points it at `kernel_main`
- calls `thread()`

Unlike `setup_thread()`, `thread()` creates normal active work. It sets
`bootstrapping` to false, increments `n_active`, allocates a normal thread
stack, creates a fresh page directory, initializes stdio descriptors, and wakes
the thread through the scheduler.

No core starts scheduling until the later start barrier completes, so the first
runnable thread cannot run before every configured core has finished its local
boot setup.

### Secondary Cores

Secondary cores are woken after core 0 has created the first runnable thread and
made the allocators safe for concurrent use.

Core 0 prepares secondary startup in this order:

- sets `start_barrier` to `CONFIG.num_cores`
- installs `boot_ipi_handler_` at `IPI_IVT_ENTRY`
- calls `physmem_sync_init()`
- calls `heap_sync_init()`
- sends an IPI to each configured secondary core with `wakeup_core(i)`
- waits until `awake_cores == CONFIG.num_cores`
- replaces the boot IPI handler with the normal `ipi_handler_`

`boot_ipi_handler_` acknowledges IPI bit 5 with `eoi 5`, disables interrupts,
sets `PSR` so `rfe` returns to kernel mode, sets `EPC` to `_start`, clears
`EFG`, and executes `rfe`. The secondary core then runs the same `_start` code
as core 0, gets its own early stack, and calls `kernel_entry()`.

For nonzero cores, `kernel_entry()` only prints the startup message and calls
`bootstrap()`. It must not run any global initialization.

### Per-Core Finalization

After the core-specific branch in `kernel_entry()`, every configured core runs
the same final setup:

- `vmem_core_init()` flushes the local TLB and sets the active PID to `0`.
- `interrupts_restore(DEFAULT_INTERRUPT_MASK)` enables global interrupts plus
  the configured PIT, PS/2, SD, IPI, and audio interrupt bits.
- `spin_barrier_sync(&start_barrier)` waits until all cores reach the same
  point.
- `event_loop()` starts the idle-thread scheduler loop.

Interrupts are enabled before the start barrier. This means a core may handle
PIT or device interrupts while waiting, but its `current_thread` has already
been initialized to the core's idle thread by `bootstrap()`.

### Idle Thread Bootstrap

`bootstrap()` creates the current core's initial scheduler context. Its
precondition is that interrupts are disabled.

The idle TCB:

- is stored in `per_core_data[core].idle_thread`
- is marked non-preemptible
- is pinned to the current core
- uses the per-core idle CLH node allocated by core 0
- becomes `per_core_data[core].current_thread`

The idle thread is not created with `thread()` and is never placed in a ready
queue. It enters `event_loop()` directly from the boot path and only switches to
normal threads selected by `schedule_next_thread()`.

### First User Program

`event_loop()` eventually schedules the `kernel_main` thread. `kernel_main()`
finds `/sbin/init` in the ext2 root filesystem and calls
`run_user_program(init, 0, NULL)`.

`run_user_program()` maps the program file, loads the ELF image, reserves the
initial user stack, builds the initial `argc` / `argv` state, and enters user
mode with `jump_to_user()`. The user-mode syscall ABI from that point is
documented in `syscalls.md`.

`/sbin/init` is responsible for starting the terminal and shell processes; see
`shell.md` for the current user-space startup topology.

### Ordering Requirements

The current boot path relies on these ordering constraints:

- `physmem_init()` must run before `heap_init()`.
- `heap_init()` must run before any heap allocation, including idle CLH node
  allocation and thread creation.
- Idle CLH nodes must be allocated before any core calls `bootstrap()`.
- `vmem_global_init()` must run before thread creation, because each normal
  thread allocates a page directory.
- `threads_init()` must run before device initialization that creates daemon
  threads, such as `ps2_init()`.
- `sd_init()` must run before `ext2_init()`.
- `ext2_init()` must run before `kernel_main` can resolve `/sbin/init`.
- `trap_init()` must run before the first user program can make syscalls.
- `physmem_sync_init()` and `heap_sync_init()` must run before secondary cores
  are woken.
- Each core must call `vmem_core_init()` before entering `event_loop()`.
