## Devices

`docs/mem_map.md` is the raw hardware contract for MMIO addresses, register layouts, and device-side semantics. This file describes the kernel-side wrappers layered on top of that contract.

### VGA

The display hardware exposes separate tile and pixel framebuffers plus tile scroll/scale, pixel scale, status, and sprite registers. `vga.h` exports the tile and pixel framebuffer pointers directly, helper routines load the text tileset and clear the screen, and `make_tiles_transparent()` can blank the tile layer so pixel output shows through underneath.

Most visible VGA behavior comes from `print.c`. When `CONFIG.use_vga` is true, `putchar_color()` writes ASCII tiles into `TILE_FB`, advances a software cursor, and implements scrolling by moving `TILE_VSCROLL` in 8-pixel rows. When `CONFIG.use_vga` is false, the same formatted-print APIs fall back to UART instead of touching the VGA hardware.

Exercised whenever a test is run with `EMU_VGA=yes`.

### UART Console

The hardware exposes one UART transmit register and one receive register, but the kernel currently wraps only transmit. `putchar_uart()`, `puts_uart()`, `printf_uart()`, and `say_uart()` write bytes directly to the UART TX MMIO register, and the higher-level `say()` / `say_color()` APIs serialize console output with `print_lock` so concurrent threads do not interleave characters.

There is no line discipline or input driver for UART RX right now. Headless test output uses this path whenever `CONFIG.use_vga` is false. Panic messages go to UART TX regardless of `CONFIG.use_vga`.

### PS/2 Keyboard

The raw PS/2 device publishes one 16-bit word at a time from MMIO: `0` means no key is pending, bit 8 marks release events, and the low byte is the guest keycode described in `docs/mem_map.md`. Printable keys use their unshifted base-key ASCII identity, left/right modifiers remain distinct, and common navigation/function keys live in a reserved non-ASCII range. The kernel keeps that raw contract for `getkey_raw()` and `waitkey_raw()`, which are the polling helpers used during boot or shutdown before the threading stack is available.

After `ps2_init()`, the normal path is interrupt-driven. Each core's PS/2 interrupt handler copies the MMIO word into a fixed per-core key buffer and signals one dedicated high-priority PS/2 worker thread through an atomic waiter handoff. The ISR records the event before detaching the waiter; the worker's post-switch callback publishes itself and consumes any event that raced with publication. This closes the sleep/wakeup race without a PIT backup or a lock in interrupt context. The worker drains all per-core buffers and republishes events into a shared `BlockingQueue`, so `getkey()` is non-blocking and `waitkey()` blocks cleanly. A per-core buffer holds 63 events; overflow drops the new event and increments the counter returned by `ps2_dropped_event_count()`.

Exercised interactively by `collatz.c`

### SD Card

The SD driver wraps the two DMA-based SD engines described in `docs/mem_map.md`. `sd_init()` must run once during boot: it clears stale status bits, issues `SD_INIT` to both controllers, waits for completion, and registers interrupt handlers. The public API is block-based only: `sd_read_blocks()` and `sd_write_blocks()` transfer whole 512-byte sectors between RAM and one drive.

Each drive has its own blocking lock, waiter slot, and pending flag. That means one transfer per drive is serialized, but drive 0 and drive 1 can make progress independently. During early boot, completion is polled with a busy-wait loop; once threading is live, the caller blocks and the SD interrupt wakes it on `DONE` or `ERR`. Driver errors are returned as negative controller error codes. The driver also prints a warning when code accesses block 0 on drive 1, because that drive currently backs the filesystem image.

Tested in `sd_drives.c`. The ext2 tests `ext_read.c`, `ext_write.c`, `ext_new_file.c`, `ext_delete.c`, and `ext_rename.c` also exercise the SD path indirectly through the filesystem.

### Audio

The OS audio path is kernel-driven. `play_audio_file(fd)` accepts a regular
file descriptor for a supported signed 16-bit little-endian mono WAV file at
25 kHz, spawns a playback worker, and streams decoded samples into the PCM
audio ring buffer. This path owns the `AUDIO_*` control block and uses the
audio low-water interrupt. The ISR and playback worker use an atomic waiter
handoff: the ISR only acknowledges the interrupt, records the event, and
detaches a waiter for deferred wakeup. It never acquires a lock. Before blocking,
the worker clears stale event state and checks the persistent `LOW_WATER` status;
the post-switch callback consumes an edge that raced with waiter publication.

### Timer

The PIT is the kernel's periodic interrupt source. `pit_init(hertz)` converts the requested frequency into a cycles-per-interrupt value for the 100 MHz hardware clock and writes that to the PIT MMIO register. The device raises the interrupt on every core, but kernel timekeeping is centralized: only core 0 increments `current_jiffies` and manages periodic MLFQ boost bookkeeping.

On every core, `pit_handler()` acknowledges the interrupt, decrements the current thread's remaining quantum, and may force a reschedule when the quantum expires and the thread is preemptible. The scheduler side of that path also wakes sleepers whose `wakeup_jiffies` deadline has passed, so the PIT is what drives `sleep()` and involuntary preemption.

Tested in `threads_sleep.c`, `threads_preempt.c`, and `mlfq_test.c`.
