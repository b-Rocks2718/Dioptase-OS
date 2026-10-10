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

After `ps2_init()`, the normal path is interrupt-driven. Each core's PS/2 interrupt handler copies the MMIO word into a fixed per-core event buffer (`EventBuf`) and signals one dedicated high-priority PS/2 worker thread through an atomic waiter handoff. The ISR records the event before detaching the waiter; the worker's post-switch callback publishes itself and consumes any event that raced with publication. This closes the sleep/wakeup race without a PIT backup or a lock in interrupt context.

Consecutive keyboard interrupts are routed to different cores, so consecutive
events land in different per-core buffers. Before its `eoi`, the ISR stamps each
event with a ticket from a global counter. The worker always publishes the
buffered event with the oldest ticket (`eventbuf_remove_oldest()`), so readers
see events in device order. The ticket order matches device order because the
device does not route the next interrupt until the current core's `eoi`; see
`debugging/input_event_cross_core_reordering.md`. Before this, the worker
drained buffers by core index and could deliver a key release before its press.

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
the SPSC ring helpers and ticket ordering are tested by `queue_test.c`. The
MMIO interrupt path is exercised interactively by `collatz.c`.

### PS/2 Mouse

The raw device publishes one 32-bit event word at a time from MMIO
(`docs/mem_map.md` "PS/2 mouse input stream"). `0` means no event is pending.
Otherwise, bits [2:0] hold the left/right/middle buttons, bit 3 is always set,
and bytes 1..3 are signed DX, DY (+down), and WHEEL (+toward the user). Motion
is relative and measured in 640x480 screen pixels. Button bits report state, not
edges. The mouse uses ISR bit 8 / IVT `0x3E0`, and `MOUSE_INT_ENABLE` is part of
`DEFAULT_INTERRUPT_MASK`.

`kernel/mouse.c` is structured like the keyboard driver. Each core's ISR reads
one word with a single 32-bit load, stamps it with a ticket, stores it in that
core's `mouse_events` `EventBuf`, issues `eoi 8`, and notifies a dedicated
high-priority worker. The worker merges the per-core rings oldest-ticket first
into a static 252-element pool and `BlockingQueue`. `getmouse()` returns the
oldest event word or `0`; there is no blocking variant. User programs reach it
through trap `57`.

The kernel does not track an absolute pointer position. Readers add up DX/DY
themselves (see `root/mousetest`). If no program reads the mouse, the pool
fills and new events are dropped and counted by `mouse_dropped_event_count()`.
Because each event carries full button state, the first event read after a drop
resynchronizes buttons, but dropped motion is lost. Programs should drain stale
events when they start.

`mouse_destroy()` follows the same quiescent-shutdown accounting as
`ps2_destroy()`. The pool handoff is tested by `mouse_queue.c`. The user trap
and the `root/crt/mouse.h` field decoders are tested by `mouse_syscall.c`. The
interactive interrupt path is exercised by the `mousetest` program
(`make run EMU_VGA=yes`, then run `mousetest` from the shell).

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

Runtime requests complete only through the SD interrupt. Every IRQ_EN command
raises exactly one completion interrupt, including completion with ERR (see
`docs/mem_map.md`), and the interrupt stays pending in `ISR` until the handler
acknowledges it (`docs/ISA.md`, `eoi`). So the ISR always finishes the request,
even when the routed core is slow to take the interrupt.

The shared kernel watchdog daemon (`kernel/watchdog.c`) enforces a software
deadline for both controllers without allocating per request. It exists only for hardware that breaks that
contract, such as a wedged controller or a lost interrupt line. It polls every
30 jiffies and gives each runtime request an implementation-defined
30,000-jiffy deadline (nominally about ten seconds with the kernel's 3,000-Hz
PIT configuration). The SD hardware does not specify a software deadline, so
these numbers are kernel policy rather than device timing guarantees. The
deadline is below the `INT_MAX` modular-time horizon. Before the deadline the
watchdog never touches a request. At the deadline it returns
`SD_DRIVER_ERR_TIMEOUT` and quarantines the drive, even if the controller
already shows DONE or ERR.

The watchdog and ISR arbitrate under the per-drive state lock, and only the
first terminal transition for the active generation may wake its caller. A
terminal result observed by the ISR quarantines the drive only if BUSY remains
set; clean DONE or terminal controller error with BUSY clear has released DMA
ownership and may admit the next request immediately.

A timed-out command is the only one whose interrupt can arrive after its
request has been finished. Quarantine keeps any new request from starting until
the ISR consumes that interrupt with BUSY clear, so the late interrupt cannot
be mistaken for completion of a newer generation, and unresolved DMA cannot
access newly staged bytes. Requests made meanwhile fail promptly with
`SD_DRIVER_ERR_QUARANTINED`. If the interrupt never arrives, quarantine is
permanent. An interrupt that arrives with no active or quarantined request is
stray, and the ISR acknowledges it and prints a diagnostic. The other drive
remains independent.

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
single-terminal-publication, controller terminal-release, quarantine,
stale-generation, and request-admission rules without requiring a deliberately
hung device. The ext2 tests `ext_read.c`,
`ext_write.c`, `ext_new_file.c`, `ext_delete.c`, and `ext_rename.c` also
exercise the SD path indirectly through the filesystem.

### Audio

The kernel audio driver (`kernel/audio.c`) only arbitrates and fills the
device's MMIO PCM ring; reading files and parsing WAV headers happen in user
mode (`root/crt/wav.h`). The device format is fixed: signed 16-bit
little-endian mono PCM at 25 kHz, from a 16 KiB ring of which 16,382 bytes are
usable (one sample stays empty so full and empty differ).

One process owns the device at a time. `audio_open()` claims it,
`audio_write()` copies as much PCM as currently fits and starts playback,
`audio_buffered()` reports how much is still queued, and `audio_close()` stops
playback and releases it. Every call is non-blocking: the owner keeps the ring
fed itself (it holds about 0.33 s), and if it falls behind the device plays
silence until refilled. Programs synchronize with the music by computing the
playback position as bytes accepted minus bytes still buffered.

Ownership is also the synchronization:

- `audio_owner` changes from NULL to a TCB only in `audio_open()`, under a
  spinlock that makes the check-then-set atomic among competing openers. It
  changes from a TCB back to NULL only in that same TCB's context:
  `audio_close()`, a successful `execv()`, or `stop()` when the process exits
  or is terminated by a signal.
- Only the owner writes the producer side of the device (`AUDIO_CTRL`,
  `AUDIO_WRITE_IDX`, `AUDIO_WATERMARK`, ring bytes) and the driver's staging
  buffer. User processes are single-threaded and signals are delivered only
  at syscall return, so an owner never runs two audio operations at once and
  these paths take no lock, including the user copy, which may demand-page.
- Release stops the device before publishing NULL, so the next owner never
  overlaps the previous one's accesses. It never blocks or spins, so `stop()`
  can run it on the signal-termination path, which has interrupts disabled.
  `stop()` releases before publishing the exit, so a parent returning from
  `wait_child()` can open the device immediately.

`audio_write()` copies the user buffer into a kernel staging buffer at the
same address mod 4 as its ring destination, so the bulk of every write uses
word copies into the ring (`kernel/audio.s`) whatever the user buffer's
alignment. The driver never enables the LOW_WATER interrupt; a handler stays
installed only to acknowledge a stray one. Kernel tests may use the low-level
`audio_output_*` helpers directly, which bypass ownership.

Tested in `audio_smoke.c` (device consumption through the low-level helpers)
and `user_misc_syscalls` (WAV validation and the ownership contract: EBADF
without ownership, EBUSY for a second opener including another process,
EINVAL, EFAULT, partial writes, and release on exit).

### Timer

The PIT is the kernel's periodic interrupt source. `pit_init(hertz)` converts the requested frequency into a cycles-per-interrupt value for the 100 MHz hardware clock and writes that to the PIT MMIO register. The device raises the interrupt on every core, but kernel timekeeping is centralized: only core 0 increments `current_jiffies` and manages periodic MLFQ boost bookkeeping.

On every core, `pit_handler()` acknowledges the interrupt, decrements the current thread's remaining quantum, and may force a reschedule when the quantum expires and the thread is preemptible. The scheduler side of that path also wakes sleepers whose `wakeup_jiffies` deadline has passed, so the PIT is what drives `sleep()` and involuntary preemption.

Tested in `threads_sleep.c`, `threads_preempt.c`, and `mlfq_test.c`.
