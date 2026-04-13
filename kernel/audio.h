#ifndef AUDIO_H
#define AUDIO_H

#include "constants.h"

#define AUDIO_RING_BASE 0x7FB8000
#define AUDIO_RING_SIZE_BYTES 0x4000
#define AUDIO_CTRL_ADDR 0x7FE5840
#define AUDIO_STATUS_ADDR 0x7FE5844
#define AUDIO_WRITE_IDX_ADDR 0x7FE5848
#define AUDIO_READ_IDX_ADDR 0x7FE584C
#define AUDIO_WATERMARK_ADDR 0x7FE5850

#define AUDIO_CTRL_ENABLE 0x1
#define AUDIO_CTRL_CLEAR_UNDERRUN 0x4

#define AUDIO_STATUS_LOW_WATER 0x2

#define AUDIO_SAMPLE_BYTES 2
#define AUDIO_WORD_BYTES 4
#define AUDIO_USABLE_BYTES 16382

/*
 * Refill once at least 1 KiB is free. That leaves roughly 0.3 seconds of
 * queued 25 kHz mono s16le audio while still giving the refill path a useful
 * chunk size to publish each time `LOW_WATER` asserts.
 */
#define AUDIO_REFILL_TRIGGER_FREE_BYTES 1024
#define AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES 15358

/*
 * Parsed PCM WAV mapped into kernel virtual memory.
 *
 * Lifetime:
 * - `bytes` points to the `mmap(...)` region that contains the whole WAV file.
 * - `data_offset` / `data_size` identify the `data` chunk inside that mapping.
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
};

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
 * - `AUDIO_STATUS.UNDERRUN` is cleared.
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
 * Load one WAV file from the ext2 root directory, map it into kernel memory,
 * and validate that it matches the fixed MMIO audio device format.
 *
 * Preconditions:
 * - `path` names one file reachable from the ext2 root directory
 * - `wav_out` points to writable kernel memory
 *
 * Postconditions:
 * - `wav_out->bytes` points at the mapped WAV bytes
 * - `wav_out->data_offset` / `wav_out->data_size` identify the validated
 *   `data` chunk payload
 * - On any lookup, mapping, or format error this helper prints a detailed
 *   audio-specific message and panics
 */
void audio_wav_load_from_root_or_panic(char* path, struct AudioWav* wav_out);

// Return the number of 16-bit PCM samples contained in the validated WAV data.
unsigned audio_wav_num_samples(struct AudioWav* wav);

// Read one signed 16-bit sample from the validated WAV data payload.
int audio_wav_read_sample_s16le(struct AudioWav* wav, unsigned sample_idx);

/*
 * Stream the WAV payload into the MMIO audio ring and block until playback has
 * drained the ring.
 *
 * Preconditions:
 * - `wav` was populated by `audio_wav_load_from_root_or_panic(...)` or another
 *   source that guarantees the same PCM format and alignment invariants
 *
 * Postconditions:
 * - Resets the device, publishes PCM bytes until the full payload is queued,
 *   waits for the ring to drain, then disables playback
 * - Panics if `LOW_WATER` asserted but a refill batch could not publish bytes
 */
void audio_wav_play_blocking(struct AudioWav* wav);

#endif // AUDIO_H
