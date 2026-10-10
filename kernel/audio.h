#ifndef AUDIO_H
#define AUDIO_H

#include "constants.h"
#include "TCB.h"

/*
 * Audio output driver.
 *
 * The device (docs/mem_map.md, audio output control block) plays signed
 * 16-bit little-endian mono PCM at 25 kHz from a 16 KiB MMIO ring. This driver
 * only arbitrates and fills that ring; reading files and parsing WAV headers
 * happen in user mode (root/crt/wav.h).
 *
 * Ownership model:
 * - At most one user process (TCB) owns the device at a time. audio_open()
 *   claims it, audio_close() releases it, and audio_release_owner() releases
 *   it when the owner exits or execs.
 * - Every user-visible audio operation is non-blocking. The owner keeps the
 *   ring fed by calling audio_write() often enough (the ring holds about 0.33 s
 *   of audio); if it falls behind, the device plays silence until refilled.
 * - The driver never enables the device's LOW_WATER interrupt.
 */

#define AUDIO_RING_BASE 0x7FB8000
#define AUDIO_RING_SIZE_BYTES 0x4000
#define AUDIO_CTRL_ADDR 0x7FE5840
#define AUDIO_STATUS_ADDR 0x7FE5844
#define AUDIO_WRITE_IDX_ADDR 0x7FE5848
#define AUDIO_READ_IDX_ADDR 0x7FE584C
#define AUDIO_WATERMARK_ADDR 0x7FE5850

#define AUDIO_CTRL_ENABLE 0x1
#define AUDIO_CTRL_IRQ 0x2

#define AUDIO_STATUS_LOW_WATER 0x2
#define AUDIO_STATUS_UNDERRUN 0x4

#define AUDIO_SAMPLE_BYTES 2
#define AUDIO_WORD_BYTES 4
// The ring must keep one sample unused so a full ring is distinguishable from
// an empty one (docs/mem_map.md).
#define AUDIO_USABLE_BYTES (AUDIO_RING_SIZE_BYTES - AUDIO_SAMPLE_BYTES)

// Register the audio ISR, quiesce the device, and initialize ownership state.
// Runs once on core 0 during kernel init, after the heap and threads exist.
void audio_init(void);

// Disable the device and destroy driver state during kernel_shutdown(). Every
// user process has exited by then, so no owner can remain.
void audio_destroy(void);

/*
 * Syscall implementations (trap codes 58-61, docs/syscalls.md). Each runs in
 * kernel mode in the calling user process's TCB, never waits for the device
 * (handle_audio_write may block only on demand paging of the user buffer), and
 * reports failure as -1 with a cause set through set_syscall_error().
 */

// Claim the device for the calling process and reset it to empty and stopped.
// Fails with EBUSY if any process, including the caller, already owns it.
int handle_audio_open(void);

// Copy up to `bytes` of PCM from user memory into the ring and start playback.
// Returns the number of bytes accepted (possibly 0 when the ring is full).
// Fails with EBADF if the caller does not own the device, EINVAL if `bytes`
// is odd, or EFAULT if the accepted prefix of `pcm` is not readable.
int handle_audio_write(char* pcm, unsigned bytes);

// Return how many accepted bytes the device has not played yet. Fails with
// EBADF if the caller does not own the device.
int handle_audio_buffered(void);

// Stop playback immediately, discard queued audio, and release the device.
// Fails with EBADF if the caller does not own the device.
int handle_audio_close(void);

/*
 * Release the device if `tcb` owns it, stopping playback and discarding queued
 * audio. Called by stop() before a terminating TCB publishes its exit and by
 * exec once the new image is committed, so ownership never outlives the
 * program that claimed it.
 *
 * Preconditions: kernel mode; `tcb` is the current TCB. Never blocks or spins,
 * so interrupts may be disabled (the signal-termination path calls stop()
 * that way).
 */
void audio_release_owner(struct TCB* tcb);

/*
 * Low-level ring helpers. These bypass ownership and are for kernel-internal
 * single-producer contexts only (kernel tests); user programs go through the
 * syscalls above.
 */

// The device reports buffered bytes modulo the ring size via read/write indices.
unsigned audio_output_buffered_bytes(unsigned write_idx, unsigned read_idx);

// Free space excludes the one 16-bit sample software must leave unused.
unsigned audio_output_free_bytes(unsigned write_idx, unsigned read_idx);

// Advance a byte index through the fixed-size ring, wrapping at the end.
unsigned audio_output_advance_idx(unsigned idx, unsigned bytes);

// Write one signed 16-bit little-endian PCM sample into the MMIO ring.
void audio_output_write_sample_s16le(int sample, unsigned write_idx);

// Read the current MMIO control/status and producer/consumer indices.
unsigned audio_output_status(void);
unsigned audio_output_write_idx(void);
unsigned audio_output_read_idx(void);

// Publish a new producer index after software has written ring bytes.
void audio_output_set_write_idx(unsigned write_idx);

/*
 * Reset the audio device state before starting a new stream.
 *
 * Preconditions:
 * - Caller is the only software producer touching the audio MMIO block.
 *
 * Postconditions:
 * - Playback is disabled.
 * - `AUDIO_WRITE_IDX` is moved to the current `AUDIO_READ_IDX`, so the ring is
 *   logically empty.
 * - Any latched `AUDIO_STATUS.UNDERRUN` state is cleared because playback is
 *   disabled.
 * - The low-water watermark is updated to `watermark_bytes`.
 */
void audio_output_reset(unsigned watermark_bytes);

// Enable or disable device consumption of queued PCM bytes. Enabling leaves
// the LOW_WATER interrupt off; nothing in the kernel waits for it.
void audio_output_enable(void);
void audio_output_disable(void);

// Convenience predicates for the documented status and empty checks.
bool audio_output_low_water(void);
bool audio_output_empty(void);

extern void audio_handler_(void);
extern void mark_audio_handled(void);

#endif // AUDIO_H
