#ifndef AUDIO_H
#define AUDIO_H

#include "constants.h"
#include "ext.h"

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
#define AUDIO_USABLE_BYTES 16382

/*
 * LOW_WATER is expressed in buffered bytes. 12286 bytes leaves 4096 bytes of
 * free ring space before waking the audio worker, so each refill gets a 4 KiB
 * copy window instead of the previous 1 KiB default.
 */
#define AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES 12286

// set up audio ISR and initialize control regs
void audio_init(void);

// disable the device and destroy audio synchronization state during shutdown
void audio_destroy(void);

/*
 * Parsed PCM WAV mapped into kernel virtual memory.
 *
 * Lifetime:
 * - `bytes` points to the `mmap(...)` region that contains the whole WAV file.
 * - `data_offset` / `data_size` identify the `data` chunk inside that mapping.
 * - `source_inumber` identifies the retained source in failure diagnostics.
 *
 * Format contract:
 * - The loader accepts only the exact audio device format documented in
 *   `docs/mem_map.md`: signed 16-bit little-endian mono PCM at 25 kHz.
 */
struct AudioWav {
  char* bytes;
  unsigned file_size;
  unsigned data_offset;
  unsigned data_size;
  unsigned source_inumber;
};

/*
 * Opaque admission request shared briefly by one syscall activation and its
 * playback worker. The worker owns and ultimately frees the request; callers
 * must not access it after audio_request_wait_until_ready() returns.
 */
struct AudioRequest;

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

// Enable or disable device consumption of queued PCM bytes.
void audio_output_enable(void);
void audio_output_disable(void);

// Convenience predicates for the documented status and empty checks.
bool audio_output_low_water(void);
bool audio_output_empty(void);

/*
 * Copy as many bytes as possible from an s16le PCM stream into the MMIO ring.
 *
 * Preconditions:
 * - `src` points to signed 16-bit little-endian mono PCM bytes
 * - `src_bytes` is even
 * - `src` is 2-byte aligned
 *
 * Postconditions:
 * - Writes raw PCM bytes into the ring without changing their sample values
 * - Publishes the final producer index exactly once
 * - Returns the number of source bytes consumed; this can be smaller than
 *   `src_bytes` when the ring does not have enough free space
 */
unsigned audio_output_fill_pcm_s16le(char* src, unsigned src_bytes);

/*
 * Map one already-open WAV Node into kernel memory and validate that it
 * matches the fixed MMIO audio device format.
 *
 * Preconditions:
 * - Kernel mode in the TCB that should own the new private VME; interrupts may
 *   be enabled and demand paging may block this thread
 * - `node` is a live regular-file wrapper owned by the caller
 * - `wav_out` points to writable kernel memory
 *
 * Postconditions:
 * - `wav_out->bytes` points at the mapped WAV bytes
 * - `wav_out->data_offset` / `wav_out->data_size` identify the validated
 *   `data` chunk payload
 * - Returns true with a live mapping on success
 * - Mapping failure or an unsupported/malformed WAV prints an actionable
 *   audio/inode/size diagnostic and returns false
 * - A parse failure releases the mapping before returning false
 */
bool audio_wav_load(struct Node* node, struct AudioWav* wav_out);

// Return the number of 16-bit PCM samples contained in the validated WAV data.
unsigned audio_wav_num_samples(struct AudioWav* wav);

// Read one signed 16-bit sample from the validated WAV data payload.
int audio_wav_read_sample_s16le(struct AudioWav* wav, unsigned sample_idx);

/*
 * Stream a validated WAV payload into the MMIO audio ring.
 *
 * Preconditions:
 * - Kernel mode; interrupts may be enabled and this counted worker may block,
 *   be preempted, or migrate between cores.
 * - `wav` describes a live private mapping owned by the current TCB and its
 *   nonempty data range contains complete 16-bit samples.
 *
 * Postconditions:
 * - Returns only after the ring drains, or after a refill cannot consume any
 *   validated source bytes.
 * - On every return, the device is disabled and audio_lock is released.
 */
bool audio_wav_play(struct AudioWav* wav);

/*
 * Clone a live regular-file Node into a heap request for the persistent audio
 * daemon. The returned request owns that clone and two initialized handoff
 * semaphores. Runs in kernel mode and may block while cloning the shared inode
 * wrapper. Returns NULL only if allocation/cloning fails.
 */
struct AudioRequest* audio_request_create(struct Node* node);

/*
 * Enqueue one request for the boot-lifetime audio daemon. Returns false when
 * the implementation-defined outstanding-request limit (queued plus playing)
 * is already reached; the caller must then destroy the unsubmitted request
 * without waiting.
 */
bool audio_request_submit(struct AudioRequest* request);

/*
 * Destroy a request that was created but never successfully submitted. Safe
 * only while no other owner exists.
 */
void audio_request_destroy_unsubmitted(struct AudioRequest* request);

/*
 * Block the current kernel-mode syscall activation until the audio daemon has
 * mapped and validated the exact private VME it will retain for playback.
 * Returns the published validation result. After return, the caller has
 * acknowledged the result and must never access `request` again; ownership is
 * exclusively the daemon's.
 */
bool audio_request_wait_until_ready(struct AudioRequest* request);

extern void audio_handler_(void);
extern void mark_audio_handled(void);

#endif // AUDIO_H
