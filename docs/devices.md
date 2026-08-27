## Devices

`docs/mem_map.md` is the raw hardware contract for MMIO addresses, register layouts, and device-side semantics. This file describes the kernel-side wrappers layered on top of that contract.

### VGA

The display hardware exposes separate tile and pixel framebuffers plus tile scroll/scale, pixel scale, status, and sprite registers. `vga.h` exports the tile and pixel framebuffer pointers directly, helper routines load the text tileset and clear the screen, and `make_tiles_transparent()` can blank the tile layer so pixel output shows through underneath.

Most visible VGA behavior comes from `print.c`. When `CONFIG.use_vga` is true,
`putchar_color()` writes ASCII tiles into `TILE_FB`, advances a software cursor,
and implements scrolling by moving `TILE_VSCROLL` in 8-pixel rows. Reaching the
end of a full screen arms the same row-entry transition as an explicit newline:
the next character clears the reused circular-buffer row and scrolls once, so
continuous text cannot overwrite the visible top row in place. The same
console lock serializes the kernel-managed tile scale and horizontal/vertical
scroll setters, including the complete read/modify/write used by the move
traps. `make_tiles_transparent()` uses a locked bulk transaction when handing
visibility to the pixel layer. Direct framebuffer/tilemap mappings still bypass
this ownership.

The console owner is pinned by disabled preemption, but long counted output,
framebuffer clears, transparency fills, and tileset loads run with the caller's
original interrupt mask. Only atomic acquisition plus owner publication and
release briefly mask interrupts. A diagnostic that interrupts a console owner
on the same core enters recursively instead of deadlocking. If the interrupted
owner is using VGA, every nested diagnostic byte is sent to UART so it cannot
touch a cursor or framebuffer transaction at an intermediate point. Display
mutators are ordinary kernel/trap-context APIs, not interrupt-context APIs.
When `CONFIG.use_vga` is false, the formatted console APIs use UART without
touching VGA state.

Exercised whenever a test is run with `EMU_VGA=yes`.

### UART Console

The hardware exposes one UART transmit register and one receive register, but
the kernel currently wraps only transmit. `putchar_uart()`, `puts_uart()`, and
the numeric `*_uart()` helpers deliberately bypass serialization for fatal
diagnostics. `printf_uart()` and `say_uart()` serialize their complete formatted
messages with the same console lock used by `printf()`, `say()`, `say_color()`,
and user stdout/stderr writes. Cross-core transactions therefore cannot
interleave. A same-core interrupt diagnostic may nest, and the panic path may
bypass the lock entirely so a failing owner can still report why it halted.

There is no line discipline or input driver for UART RX right now. Headless test
output uses this path whenever `CONFIG.use_vga` is false. Panic messages go to
UART TX regardless of `CONFIG.use_vga`.

The UART RX interrupt source stays masked by default. The registered handler
acknowledges the ISA edge and returns without consuming receive data, so
enabling the source is a no-op stub until a real input driver exists. The same
policy applies to VGA VBLANK: the handler acknowledges and returns, and the
source remains masked unless a future refresh consumer enables it.

### PS/2 Keyboard

The raw PS/2 device publishes one 16-bit word at a time from MMIO: `0` means no key is pending, bit 8 marks release events, and the low byte is the guest keycode described in `docs/mem_map.md`. Printable keys use their unshifted base-key ASCII identity, left/right modifiers remain distinct, and common navigation/function keys live in a reserved non-ASCII range. The kernel keeps that raw contract for `getkey_raw()` and `waitkey_raw()`, which are the polling helpers used during boot or shutdown before the threading stack is available.

After `ps2_init()`, the normal path is interrupt-driven. Each core's PS/2 interrupt handler copies the MMIO word into a fixed per-core key buffer and signals one dedicated high-priority PS/2 worker thread through an atomic waiter handoff. The ISR records the event before detaching the waiter; the worker's post-switch callback publishes itself and consumes any event that raced with publication. This closes the sleep/wakeup race without a PIT backup or a lock in interrupt context.

The worker drains all per-core buffers into a fixed static event pool and a
shared `BlockingQueue`, so `getkey()` is non-blocking and `waitkey()` blocks
cleanly without per-event heap allocation. Each per-core SPSC ring has 63
usable entries, and the shared pool has one element for every usable ring slot:
252 elements with the current four-core maximum. A reader copies an event and
then recycles its element to the private free queue. If either an ISR ring or
the shared pool is full, the new nonzero event is dropped; the worker never
blocks waiting for pool storage. `ps2_dropped_event_count()` is the aggregate
number of drops at both boundaries since initialization, modulo 2^32. This
loss policy preserves already queued FIFO order but does not provide
backpressure or guaranteed delivery to an indefinitely slow reader.

During shutdown, every core first disables interrupts and reaches the global
barrier. With the worker and readers quiescent, `ps2_destroy()` drains both
queues, requires all 252 static elements to be accounted for, and destroys only
the synchronization state; it never passes an event element to `free()`.

The bounded worker handoff is tested deterministically by `ps2_queue.c`, and
the SPSC ring helpers are tested by `queue_test.c`. The MMIO interrupt path is
exercised interactively by `collatz.c`.

### SD Card

The SD driver wraps the two DMA-based SD engines described in `docs/mem_map.md`. `sd_init()` must run once during boot: it initializes per-drive state, resolves controller status, issues `SD_INIT` to both controllers, waits for completion, and registers interrupt handlers. The public API is block-based only: `sd_read_blocks()` and `sd_write_blocks()` transfer whole 512-byte sectors between ordinary physical RAM and one drive.

Admission validates the complete request before touching MMIO. The drive must be
0 or 1, the starting block must be nonnegative, and the block count must be
positive. The DMA buffer must be non-NULL and four-byte aligned because the
controller discards its low two address bits. Block/count arithmetic must be
representable, and the complete `count * 512` byte span must remain below the
`0x07FB8000` start of the documented MMIO window. Although the raw controller
can target MMIO, this wrapper rejects such buffers because CPU-side staging
cannot preserve DMA MMIO side effects. A malformed internal request is
reported with operation, drive, block, count, buffer, and rejection reason and
returns `SD_DRIVER_ERR_INVALID_REQUEST`; it does not program the controller.

Each drive has its own blocking command lock, interrupt waiter, generation
state, and tiny interrupt-safe state lock. One transfer per drive is serialized,
but drive 0 and drive 1 can make progress independently. Before START or
SD_INIT, the driver resolves sticky DONE/ERR/error state and rejects BUSY
promptly instead of issuing the hardware's BUSY-rejected command and then
waiting for a DONE bit that the hardware contract says will not be set.

Once threading is live, normal completion remains interrupt-driven. Waiter
publication occurs only in the post-context-switch callback, after the outgoing
TCB is no longer running or queued. The ISR and callback use the
sequentially-consistent `InterruptWaiter` handoff, so an early interrupt becomes
one pending event and cannot enqueue the TCB twice.

Each drive owns one permanent, aligned 4 KiB bounce page. The command lock
splits larger API requests into at most eight-sector commands. Writes are
copied into that page before START; reads are copied to the caller only after a
successful terminal result. Hardware therefore never retains a caller buffer
address. If non-atomic DMA remains BUSY after a software timeout, it can touch
only the quarantined drive's bounce page, so returning cannot create a
use-after-free or post-return overwrite of caller memory.

One persistent watchdog supplies an independent PIT-driven completion path for
both controllers without allocating per request. It polls every 30 jiffies and
gives each runtime request an implementation-defined 30,000-jiffy deadline
(nominally about ten seconds with the kernel's 3,000-Hz PIT configuration). The
SD hardware does not specify a software deadline, so these numbers are kernel
policy rather than device timing guarantees. The deadline is below the
`INT_MAX` modular-time horizon. If status becomes terminal but the SD interrupt
is missing, the watchdog publishes the controller result. If neither status nor
interrupt progresses by the deadline, it returns `SD_DRIVER_ERR_TIMEOUT` while
retaining the bounce page under quarantine.

The watchdog and ISR arbitrate under the per-drive state lock, and only the
first terminal transition for the active generation may wake its caller. A
watchdog-observed terminal result quarantines the drive only if BUSY remains
set; clean DONE or terminal controller error with BUSY clear has released DMA
ownership and may admit the next request immediately. The driver records the
outstanding interrupt provenance, so its delayed handler either clears the old
sticky terminal state or, if a newer command has already completed, safely
handles that active generation. A delayed old edge observed while the newer
command is nonterminal is acknowledged without waking it or clearing its
status.

A nonterminal software deadline always quarantines the drive until terminal
hardware state with BUSY clear and the late interrupt are acknowledged. Later
requests fail promptly with `SD_DRIVER_ERR_QUARANTINED`; if the device never
completes, quarantine remains permanent. Consequently unresolved DMA cannot
access newly staged bytes, and a late IRQ cannot be mistaken for completion of
a newer generation. The other drive remains independent.

Early boot cannot use scheduler or PIT wakeups, so it polls without IRQ_EN for
at most 16,777,216 status reads. This implementation-defined operation budget
is finite but intentionally makes no elapsed-time claim. Exhaustion records the
drive, generation, command metadata, status, and controller error before the
required-device initialization failure stops boot.

Controller errors retain the documented negative values `-1` through `-6`.
Software validation, timeout, inconsistent-status, and quarantine errors use
the separate named values in `kernel/sd_driver.h`. The driver also prints a
warning when code accesses block 0 on drive 1, because that drive currently
backs the filesystem image.

Tested in `sd_drives.c` and `sd_validation.c`. The latter exercises generation,
single-terminal-publication, watchdog terminal-release, quarantine,
stale-generation, and request-admission rules without requiring a deliberately
hung device. The ext2 tests `ext_read.c`,
`ext_write.c`, `ext_new_file.c`, `ext_delete.c`, and `ext_rename.c` also
exercise the SD path indirectly through the filesystem.

### Audio

The OS audio path is kernel-driven. `play_audio_file(fd)` accepts a regular
file descriptor for a supported signed 16-bit little-endian mono WAV file at
25 kHz and submits it to one persistent high-priority audio daemon. Kernel VM
mappings belong to one TCB, so the daemon reserves and parses its own private
mapping. A ready/acknowledgement handoff keeps the request alive while the
syscall waits for that validation result. Mapping failure, malformed or
truncated RIFF/chunk metadata, unsupported fixed-format fields, empty or
sample-misaligned data, and four already-outstanding requests (queued plus the
request currently playing) are ordinary `-1` syscall results. A successful
syscall then returns while playback continues asynchronously from the same
retained mapping, independently of the caller's descriptor lifetime.

The daemon drains a spin-protected request queue and parks through a
sequentially-consistent `InterruptWaiter` handoff. A submitter publishes the
request before signalling, so a notification racing with the daemon's
post-context-switch TCB publication cannot be lost or enqueue the daemon twice.
The daemon allocates its private page directory lazily after removing its first
accepted request. Each accepted request holds one asynchronous-work reference
from publication through that initialization, validation/playback, mapping and
Node cleanup, request destruction, and capacity release. Kernel event loops
therefore remain live until all retained audio resources are gone, while an
unused daemon can be discarded without abandoning a page allocation or live
allocator operation. During globally quiescent shutdown, the empty request
queue is destroyed, the daemon's boot-lifetime TCB is detached from its waiter,
and its private address space, if it was ever created, is explicitly reclaimed
before VM/physmem teardown.

This path owns the `AUDIO_*` control block. The bounded audio ISR acknowledges
each enabled low-water edge, but playback progress does not depend on an
interrupt wake: the daemon polls LOW_WATER and the ring indices once per jiffy
with an implementation-defined 30,000-jiffy elapsed deadline (same modular
half-range ordering as sleep/SD). If the device never reaches LOW_WATER or
never drains within that budget, the daemon disables the device, releases the
audio lock, cleans the retained mapping, and accepts the next queued request.
Every refill of a validated source must also consume at least one complete
sample; a zero-byte refill is treated as the same asynchronous failure class.

### Timer

The PIT is the kernel's periodic interrupt source. `pit_init(hertz)` converts the requested frequency into a cycles-per-interrupt value for the 100 MHz hardware clock and writes that to the PIT MMIO register. The device raises the interrupt on every core, but kernel timekeeping is centralized: only core 0 increments `current_jiffies` and manages periodic MLFQ boost bookkeeping.

On every core, `pit_handler()` acknowledges the interrupt, decrements the current thread's remaining quantum, and may force a reschedule when the quantum expires and the thread is preemptible. The scheduler side of that path also wakes sleepers whose `wakeup_jiffies` deadline has passed, so the PIT is what drives `sleep()` and involuntary preemption.

Tested in `threads_sleep.c`, `threads_preempt.c`, and `mlfq_test.c`.
