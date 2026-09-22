#include "audio.h"

#include "debug.h"
#include "ext.h"
#include "print.h"
#include "threads.h"
#include "vmem.h"
#include "ivt.h"
#include "blocking_lock.h"
#include "queue.h"
#include "per_core.h"
#include "heap.h"
#include "interrupt_waiter.h"
#include "interrupts.h"
#include "scheduler.h"
#include "semaphore.h"
#include "pit.h"
#include "constants.h"
#include "machine.h"
#include "TCB.h"

struct BlockingLock audio_lock;

/*
 * One persistent HIGH_PRIORITY daemon drains this queue. Admission refuses a
 * new request when AUDIO_MAX_QUEUED_REQUESTS are already waiting or playing,
 * so play_audio_file cannot spawn unbounded workers.
 */
#define AUDIO_MAX_QUEUED_REQUESTS 4
#define AUDIO_PROGRESS_TIMEOUT_JIFFIES 30000

static struct GenericSpinQueue audio_request_queue;
static struct InterruptWaiter audio_request_waiter;
static int audio_queued_count;
static bool audio_daemon_started;
static struct TCB* audio_daemon_tcb;

// Audio MMIO from docs/mem_map.md. STATUS and READ_IDX are hardware read-only.
static volatile char * const AUDIO_RING = (volatile char *)AUDIO_RING_BASE;
static volatile unsigned * const AUDIO_CTRL = (volatile unsigned *)AUDIO_CTRL_ADDR;
static const volatile unsigned * const AUDIO_STATUS =
    (const volatile unsigned *)AUDIO_STATUS_ADDR;
static volatile unsigned * const AUDIO_WRITE_IDX =
    (volatile unsigned *)AUDIO_WRITE_IDX_ADDR;
static const volatile unsigned * const AUDIO_READ_IDX =
    (const volatile unsigned *)AUDIO_READ_IDX_ADDR;
static volatile unsigned * const AUDIO_WATERMARK =
    (volatile unsigned *)AUDIO_WATERMARK_ADDR;

#define WAV_RIFF_HEADER_BYTES 12
#define WAV_RIFF_ID_OFFSET 0
#define WAV_RIFF_SIZE_OFFSET 4
#define WAV_WAVE_ID_OFFSET 8
#define WAV_RIFF_SIZE_PREFIX_BYTES 8
#define WAV_FORM_TYPE_BYTES 4
#define WAV_CHUNK_HEADER_BYTES 8
#define WAV_CHUNK_SIZE_OFFSET 4
#define WAV_FMT_MIN_BYTES 16
#define WAV_FMT_AUDIO_FORMAT_OFFSET 0
#define WAV_FMT_CHANNELS_OFFSET 2
#define WAV_FMT_SAMPLE_RATE_OFFSET 4
#define WAV_FMT_BYTE_RATE_OFFSET 8
#define WAV_FMT_BLOCK_ALIGN_OFFSET 12
#define WAV_FMT_BITS_PER_SAMPLE_OFFSET 14
#define WAV_PCM_FORMAT 1
#define WAV_EXPECTED_CHANNELS 1
#define WAV_EXPECTED_SAMPLE_RATE 25000
#define WAV_EXPECTED_BITS_PER_SAMPLE 16
#define WAV_EXPECTED_BLOCK_ALIGN 2
#define WAV_EXPECTED_BYTE_RATE 50000

#define WAV_RIFF_ID_LE 0x46464952
#define WAV_WAVE_ID_LE 0x45564157

// Classify the validation failures that can reject a WAV stream.
enum AudioWavErrorCode {
  AUDIO_WAV_ERROR_NONE = 0,
  AUDIO_WAV_ERROR_FILE_TOO_SMALL,
  AUDIO_WAV_ERROR_RIFF_SIGNATURE,
  AUDIO_WAV_ERROR_WAVE_SIGNATURE,
  AUDIO_WAV_ERROR_RIFF_SIZE_TOO_SMALL,
  AUDIO_WAV_ERROR_RIFF_SIZE_PAST_FILE,
  AUDIO_WAV_ERROR_CHUNK_HEADER_TRUNCATED,
  AUDIO_WAV_ERROR_CHUNK_SIZE_OVERFLOW,
  AUDIO_WAV_ERROR_CHUNK_PAST_RIFF,
  AUDIO_WAV_ERROR_CHUNK_PADDING_PAST_RIFF,
  AUDIO_WAV_ERROR_FMT_TOO_SHORT,
  AUDIO_WAV_ERROR_AUDIO_FORMAT,
  AUDIO_WAV_ERROR_CHANNELS,
  AUDIO_WAV_ERROR_SAMPLE_RATE,
  AUDIO_WAV_ERROR_BYTE_RATE,
  AUDIO_WAV_ERROR_BLOCK_ALIGN,
  AUDIO_WAV_ERROR_BITS_PER_SAMPLE,
  AUDIO_WAV_ERROR_DATA_BEFORE_FMT,
  AUDIO_WAV_ERROR_DATA_EMPTY,
  AUDIO_WAV_ERROR_DATA_MISALIGNED,
  AUDIO_WAV_ERROR_DATA_MISSING,
};

// Describe why a WAV stream was rejected before playback.
struct AudioWavError {
  enum AudioWavErrorCode code;
  unsigned offset;
  unsigned got;
  unsigned expected;
};

/*
 * One request crosses exactly one syscall thread and the persistent playback
 * daemon.
 *
 * Ownership and synchronization:
 * - The daemon frees this allocation after playback/cleanup completes.
 * - node is an independent clone released by the daemon on every path.
 * - validation_succeeded is a 32-bit word so the architecture's word-sized
 *   sequentially-consistent atomic operations can publish it explicitly.
 * - ready transfers the published result to the caller; caller_acknowledged
 *   transfers exclusive request ownership back to the daemon.
 * - owner_count starts at two. The caller releases only after its final
 *   sem_up() returns; the daemon releases only after all parsing/playback and
 *   mapping/Node cleanup. The 1->0 releaser alone destroys both semaphores.
 * - link is the first member so the request can enter BlockingQueue.
 */
struct AudioRequest {
  struct GenericQueueElement link;
  struct Node* node;
  struct Semaphore ready;
  struct Semaphore caller_acknowledged;
  int validation_succeeded;
  int owner_count;
  int handoff_finalized;
};

// Return whether the wrapping tick counter has reached a playback deadline.
static bool audio_deadline_reached(unsigned now, unsigned deadline){
  unsigned delta = now - deadline;
  return delta == 0 || (delta & (INT_MAX + 1U)) == 0;
}

static bool audio_request_release_owner(struct AudioRequest* request);
static void audio_daemon(void* unused);
static void audio_process_request(struct AudioRequest* request);

// register audio isr and initialize control regs
void audio_init(void){
  register_handler(audio_handler_, (void*)AUDIO_IVT_ENTRY);
  audio_output_reset(AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES);
  blocking_lock_init(&audio_lock);
  generic_spin_queue_init(&audio_request_queue);
  interrupt_waiter_init(&audio_request_waiter);
  __atomic_store_n(&audio_queued_count, 0);
  audio_daemon_started = false;
  __atomic_store_n((int*)&audio_daemon_tcb, (int)NULL);

  struct Fun* daemon_fun = leak(sizeof(struct Fun));
  daemon_fun->func = audio_daemon;
  daemon_fun->arg = NULL;
  setup_thread(daemon_fun, HIGH_PRIORITY, ANY_CORE);
  audio_daemon_started = true;
}

// to be called only from kernel_shutdown
void audio_destroy(void){
  /*
   * kernel_async_work_count keeps every core in event_loop() until the daemon
   * has cleaned all accepted requests. Once every core reaches the shutdown
   * barrier, the queue is empty and the boot-lifetime daemon can no longer
   * execute. Before its first request, every preemptible daemon continuation
   * owns no address-space allocation; request processing, including lazy
   * address-space initialization, is protected by the asynchronous-work count.
   */
  audio_output_disable();
  generic_spin_queue_destroy(&audio_request_queue);
  interrupt_waiter_init(&audio_request_waiter);
  blocking_lock_destroy(&audio_lock);

  struct TCB* daemon = (struct TCB*)__atomic_load_n((int*)&audio_daemon_tcb);
  if (daemon != NULL){
    assert_always(daemon->is_daemon,
      "audio destroy: recorded playback TCB must be a persistent daemon.\n");
    assert_always(daemon->vme_list == NULL,
      "audio destroy: playback daemon retained a VME after asynchronous work reached zero.\n");

    /*
     * Every core is in its PID-0 idle shutdown context and this boot-lifetime
     * TCB can never resume. Reclaim its finite address-space pages, if audio
     * ever needed them, before VM and physmem teardown. A zero PID is the
     * expected state when no request reached the daemon.
     */
    if (daemon->pid != 0){
      vmem_destroy_address_space(daemon);
      daemon->pid = 0;
    }
    __atomic_store_n((int*)&audio_daemon_tcb, (int)NULL);
  }
  audio_daemon_started = false;
}

// Copy an even number of aligned PCM bytes into the fixed ring with ld/sd.
extern unsigned audio_copy_even_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

// Copy a multiple of 4 aligned PCM bytes into the fixed ring with lw/sw.
extern unsigned audio_copy_word_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

// Decode a little-endian 16-bit value from an untrusted WAV header.
static unsigned read_u16_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8);
}

// Decode a little-endian 32-bit value from an untrusted WAV header.
static unsigned read_u32_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8) |
         ((unsigned)(unsigned char)bytes[2] << 16) |
         ((unsigned)(unsigned char)bytes[3] << 24);
}

// Return whether four bytes match a WAV chunk identifier.
static bool chunk_id_is(char* bytes, char a, char b, char c, char d){
  return bytes[0] == a && bytes[1] == b && bytes[2] == c && bytes[3] == d;
}

// Store a WAV validation failure and emit its diagnostic context.
static bool audio_wav_reject(struct AudioWavError* error,
    enum AudioWavErrorCode code, unsigned offset,
    unsigned got, unsigned expected){
  error->code = code;
  error->offset = offset;
  error->got = got;
  error->expected = expected;
  return false;
}

// Map a WAV validation code to its diagnostic name.
static char* audio_wav_error_name(enum AudioWavErrorCode code){
  switch (code){
    case AUDIO_WAV_ERROR_FILE_TOO_SMALL: return "file_too_small";
    case AUDIO_WAV_ERROR_RIFF_SIGNATURE: return "riff_signature";
    case AUDIO_WAV_ERROR_WAVE_SIGNATURE: return "wave_signature";
    case AUDIO_WAV_ERROR_RIFF_SIZE_TOO_SMALL: return "riff_size_too_small";
    case AUDIO_WAV_ERROR_RIFF_SIZE_PAST_FILE: return "riff_size_past_file";
    case AUDIO_WAV_ERROR_CHUNK_HEADER_TRUNCATED: return "chunk_header_truncated";
    case AUDIO_WAV_ERROR_CHUNK_SIZE_OVERFLOW: return "chunk_size_overflow";
    case AUDIO_WAV_ERROR_CHUNK_PAST_RIFF: return "chunk_past_riff";
    case AUDIO_WAV_ERROR_CHUNK_PADDING_PAST_RIFF: return "chunk_padding_past_riff";
    case AUDIO_WAV_ERROR_FMT_TOO_SHORT: return "fmt_too_short";
    case AUDIO_WAV_ERROR_AUDIO_FORMAT: return "unsupported_audio_format";
    case AUDIO_WAV_ERROR_CHANNELS: return "unsupported_channels";
    case AUDIO_WAV_ERROR_SAMPLE_RATE: return "unsupported_sample_rate";
    case AUDIO_WAV_ERROR_BYTE_RATE: return "unsupported_byte_rate";
    case AUDIO_WAV_ERROR_BLOCK_ALIGN: return "unsupported_block_align";
    case AUDIO_WAV_ERROR_BITS_PER_SAMPLE: return "unsupported_bits_per_sample";
    case AUDIO_WAV_ERROR_DATA_BEFORE_FMT: return "data_before_fmt";
    case AUDIO_WAV_ERROR_DATA_EMPTY: return "data_empty";
    case AUDIO_WAV_ERROR_DATA_MISALIGNED: return "data_misaligned";
    case AUDIO_WAV_ERROR_DATA_MISSING: return "data_missing";
    case AUDIO_WAV_ERROR_NONE: return "none";
  }

  return "unknown";
}

// Report a stored WAV validation failure through the kernel diagnostic path.
static void audio_wav_report_error(struct Node* wav_node, unsigned wav_size,
    struct AudioWavError* error){
  void* args[6];
  args[0] = (void*)wav_node->cached->inumber;
  args[1] = (void*)wav_size;
  args[2] = audio_wav_error_name(error->code);
  args[3] = (void*)error->offset;
  args[4] = (void*)error->got;
  args[5] = (void*)error->expected;
  say("audio wav validate: inode=%d size=%d error=%s offset=0x%X got=0x%X expected=0x%X\n",
    args);
}

/*
 * Parse untrusted user-file bytes without invoking kernel invariants.
 *
 * Preconditions:
 * - Kernel mode; interrupts may be enabled and the current worker may block on
 *   demand paging while reading `wav`.
 * - `wav[0..wav_size)` is one live private VME owned by the current TCB.
 * - wav_out and error point to writable kernel memory.
 *
 * Postconditions:
 * - Every multi-byte field is read only after the containing range is proven
 *   to lie inside both the mapped file and its declared RIFF extent.
 * - Success describes one nonempty, sample-aligned payload matching the fixed
 *   MMIO format. Failure records a structured ordinary-input diagnostic and
 *   leaves all kernel locks untouched.
 */
static bool audio_wav_parse(char* wav,
    unsigned wav_size, struct AudioWav* wav_out,
    struct AudioWavError* error){
  bool saw_fmt = false;
  unsigned offset = WAV_RIFF_HEADER_BYTES;
  unsigned riff_size;
  unsigned riff_end;

  error->code = AUDIO_WAV_ERROR_NONE;
  error->offset = 0;
  error->got = 0;
  error->expected = 0;

  if (wav_size < WAV_RIFF_HEADER_BYTES){
    return audio_wav_reject(error, AUDIO_WAV_ERROR_FILE_TOO_SMALL,
      WAV_RIFF_ID_OFFSET, wav_size, WAV_RIFF_HEADER_BYTES);
  }
  if (!chunk_id_is(wav, 'R', 'I', 'F', 'F')){
    return audio_wav_reject(error, AUDIO_WAV_ERROR_RIFF_SIGNATURE,
      WAV_RIFF_ID_OFFSET, read_u32_le(wav), WAV_RIFF_ID_LE);
  }
  if (!chunk_id_is(wav + WAV_WAVE_ID_OFFSET, 'W', 'A', 'V', 'E')){
    return audio_wav_reject(error, AUDIO_WAV_ERROR_WAVE_SIGNATURE,
      WAV_WAVE_ID_OFFSET, read_u32_le(wav + WAV_WAVE_ID_OFFSET),
      WAV_WAVE_ID_LE);
  }

  riff_size = read_u32_le(wav + WAV_RIFF_SIZE_OFFSET);
  if (riff_size < WAV_FORM_TYPE_BYTES){
    return audio_wav_reject(error, AUDIO_WAV_ERROR_RIFF_SIZE_TOO_SMALL,
      WAV_RIFF_SIZE_OFFSET, riff_size, WAV_FORM_TYPE_BYTES);
  }
  if (riff_size > wav_size - WAV_RIFF_SIZE_PREFIX_BYTES){
    return audio_wav_reject(error, AUDIO_WAV_ERROR_RIFF_SIZE_PAST_FILE,
      WAV_RIFF_SIZE_OFFSET, riff_size,
      wav_size - WAV_RIFF_SIZE_PREFIX_BYTES);
  }
  // The preceding subtraction check proves this addition cannot overflow.
  riff_end = WAV_RIFF_SIZE_PREFIX_BYTES + riff_size;

  wav_out->bytes = wav;
  wav_out->file_size = wav_size;
  wav_out->data_offset = 0;
  wav_out->data_size = 0;

  while (offset < riff_end){
    unsigned chunk_header_bytes = riff_end - offset;
    if (chunk_header_bytes < WAV_CHUNK_HEADER_BYTES){
      return audio_wav_reject(error,
        AUDIO_WAV_ERROR_CHUNK_HEADER_TRUNCATED, offset,
        chunk_header_bytes, WAV_CHUNK_HEADER_BYTES);
    }

    char* chunk = wav + offset;
    unsigned chunk_size = read_u32_le(chunk + WAV_CHUNK_SIZE_OFFSET);
    unsigned chunk_data_offset = offset + WAV_CHUNK_HEADER_BYTES;
    unsigned padded_chunk_size;
    unsigned chunk_capacity = riff_end - chunk_data_offset;

    // Validate the optional RIFF pad-byte addition before performing it.
    if ((chunk_size & 1) != 0 && chunk_size == UINT_MAX){
      return audio_wav_reject(error, AUDIO_WAV_ERROR_CHUNK_SIZE_OVERFLOW,
        offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, UINT_MAX - 1);
    }
    padded_chunk_size = chunk_size + (chunk_size & 1);

    if (chunk_size > chunk_capacity){
      return audio_wav_reject(error, AUDIO_WAV_ERROR_CHUNK_PAST_RIFF,
        offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, chunk_capacity);
    }
    if (padded_chunk_size > chunk_capacity){
      return audio_wav_reject(error,
        AUDIO_WAV_ERROR_CHUNK_PADDING_PAST_RIFF,
        offset + WAV_CHUNK_SIZE_OFFSET, padded_chunk_size, chunk_capacity);
    }

    if (chunk_id_is(chunk, 'f', 'm', 't', ' ')){
      unsigned audio_format;
      unsigned num_channels;
      unsigned sample_rate;
      unsigned byte_rate;
      unsigned block_align;
      unsigned bits_per_sample;

      if (chunk_size < WAV_FMT_MIN_BYTES){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_FMT_TOO_SHORT,
          offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, WAV_FMT_MIN_BYTES);
      }

      audio_format = read_u16_le(wav + chunk_data_offset +
        WAV_FMT_AUDIO_FORMAT_OFFSET);
      num_channels = read_u16_le(wav + chunk_data_offset +
        WAV_FMT_CHANNELS_OFFSET);
      sample_rate = read_u32_le(wav + chunk_data_offset +
        WAV_FMT_SAMPLE_RATE_OFFSET);
      byte_rate = read_u32_le(wav + chunk_data_offset +
        WAV_FMT_BYTE_RATE_OFFSET);
      block_align = read_u16_le(wav + chunk_data_offset +
        WAV_FMT_BLOCK_ALIGN_OFFSET);
      bits_per_sample = read_u16_le(wav + chunk_data_offset +
        WAV_FMT_BITS_PER_SAMPLE_OFFSET);

      if (audio_format != WAV_PCM_FORMAT){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_AUDIO_FORMAT,
          chunk_data_offset + WAV_FMT_AUDIO_FORMAT_OFFSET,
          audio_format, WAV_PCM_FORMAT);
      }
      if (num_channels != WAV_EXPECTED_CHANNELS){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_CHANNELS,
          chunk_data_offset + WAV_FMT_CHANNELS_OFFSET,
          num_channels, WAV_EXPECTED_CHANNELS);
      }
      if (sample_rate != WAV_EXPECTED_SAMPLE_RATE){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_SAMPLE_RATE,
          chunk_data_offset + WAV_FMT_SAMPLE_RATE_OFFSET,
          sample_rate, WAV_EXPECTED_SAMPLE_RATE);
      }
      if (bits_per_sample != WAV_EXPECTED_BITS_PER_SAMPLE){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_BITS_PER_SAMPLE,
          chunk_data_offset + WAV_FMT_BITS_PER_SAMPLE_OFFSET, bits_per_sample,
          WAV_EXPECTED_BITS_PER_SAMPLE);
      }
      if (block_align != WAV_EXPECTED_BLOCK_ALIGN){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_BLOCK_ALIGN,
          chunk_data_offset + WAV_FMT_BLOCK_ALIGN_OFFSET,
          block_align, WAV_EXPECTED_BLOCK_ALIGN);
      }
      if (byte_rate != WAV_EXPECTED_BYTE_RATE){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_BYTE_RATE,
          chunk_data_offset + WAV_FMT_BYTE_RATE_OFFSET,
          byte_rate, WAV_EXPECTED_BYTE_RATE);
      }

      saw_fmt = true;
    } else if (chunk_id_is(chunk, 'd', 'a', 't', 'a')){
      if (!saw_fmt){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_DATA_BEFORE_FMT,
          offset, 0, 1);
      }
      if (chunk_size == 0){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_DATA_EMPTY,
          offset + WAV_CHUNK_SIZE_OFFSET, 0, AUDIO_SAMPLE_BYTES);
      }
      if ((chunk_size % WAV_EXPECTED_BLOCK_ALIGN) != 0 ||
          (chunk_data_offset % WAV_EXPECTED_BLOCK_ALIGN) != 0){
        return audio_wav_reject(error, AUDIO_WAV_ERROR_DATA_MISALIGNED,
          chunk_data_offset, chunk_size, WAV_EXPECTED_BLOCK_ALIGN);
      }
      wav_out->data_offset = chunk_data_offset;
      wav_out->data_size = chunk_size;
      return true;
    }

    // padded_chunk_size <= chunk_capacity proves this addition cannot overflow.
    offset = chunk_data_offset + padded_chunk_size;
  }

  return audio_wav_reject(error, AUDIO_WAV_ERROR_DATA_MISSING,
    riff_end, 0, 1);
}

// Compute the number of PCM bytes currently queued in the hardware ring.
unsigned audio_output_buffered_bytes(unsigned write_idx, unsigned read_idx){
  if (write_idx >= read_idx){
    return write_idx - read_idx;
  }
  return AUDIO_RING_SIZE_BYTES - (read_idx - write_idx);
}

// Compute writable PCM capacity while preserving the ring's empty slot.
unsigned audio_output_free_bytes(unsigned write_idx, unsigned read_idx){
  return AUDIO_USABLE_BYTES - audio_output_buffered_bytes(write_idx, read_idx);
}

// Advance a PCM ring index with wraparound.
unsigned audio_output_advance_idx(unsigned idx, unsigned bytes){
  idx += bytes;
  if (idx >= AUDIO_RING_SIZE_BYTES){
    idx -= AUDIO_RING_SIZE_BYTES;
  }
  return idx;
}

// Write one signed 16-bit little-endian sample into the PCM ring.
void audio_output_write_sample_s16le(int sample, unsigned write_idx){
  unsigned encoded = (unsigned short)sample;
  unsigned next_idx = audio_output_advance_idx(write_idx, 1);

  AUDIO_RING[write_idx] = encoded & 0xFF;
  AUDIO_RING[next_idx] = (encoded >> 8) & 0xFF;
}

// Read the audio device status register.
unsigned audio_output_status(void){
  return *AUDIO_STATUS;
}

// Read the producer index published by the audio device.
unsigned audio_output_write_idx(void){
  return *AUDIO_WRITE_IDX;
}

// Read the consumer index published by the audio device.
unsigned audio_output_read_idx(void){
  return *AUDIO_READ_IDX;
}

// Publish a new producer index after PCM bytes have been written.
void audio_output_set_write_idx(unsigned write_idx){
  *AUDIO_WRITE_IDX = write_idx;
}

// Reset the PCM ring and program its low-water watermark.
void audio_output_reset(unsigned watermark_bytes){
  unsigned read_idx = *AUDIO_READ_IDX;

  *AUDIO_CTRL = 0;
  *AUDIO_WATERMARK = watermark_bytes;
  *AUDIO_WRITE_IDX = read_idx;
}

// Enable audio playback after the ring has been prepared.
void audio_output_enable(void){
  *AUDIO_CTRL = AUDIO_CTRL_ENABLE | AUDIO_CTRL_IRQ;
}

// Disable audio playback without changing queued PCM data.
void audio_output_disable(void){
  *AUDIO_CTRL = 0;
}

// Return whether the device reports that its PCM ring is below watermark.
bool audio_output_low_water(void){
  return ((*AUDIO_STATUS) & AUDIO_STATUS_LOW_WATER) != 0;
}

// Return whether the hardware has consumed every queued PCM byte.
bool audio_output_empty(void){
  return *AUDIO_READ_IDX == *AUDIO_WRITE_IDX;
}

// Return the byte offset within one four-byte stereo sample.
static unsigned audio_ring_ptr_mod4(unsigned write_idx){
  return (AUDIO_RING_BASE + write_idx) & (AUDIO_WORD_BYTES - 1);
}

// Append one silent stereo sample and return the next ring index.
static unsigned audio_write_silence_sample(unsigned write_idx){
  AUDIO_RING[write_idx] = 0;
  AUDIO_RING[audio_output_advance_idx(write_idx, 1)] = 0;
  return audio_output_advance_idx(write_idx, AUDIO_SAMPLE_BYTES);
}

// Copy as many complete PCM samples as fit into the output ring.
unsigned audio_output_fill_pcm_s16le(char* src, unsigned src_bytes){
  unsigned read_idx = *AUDIO_READ_IDX;
  unsigned write_idx = *AUDIO_WRITE_IDX;
  unsigned free_bytes = audio_output_free_bytes(write_idx, read_idx);
  unsigned copy_bytes = src_bytes;
  unsigned consumed_bytes = 0;
  unsigned word_copy_bytes;
  unsigned tail_copy_bytes;

  /*
   * Snapshot the consumer index once per batch, then publish as many PCM bytes
   * as fit before updating AUDIO_WRITE_IDX. Because the source payload already
   * matches the device format exactly, the hot path copies raw bytes directly
   * into MMIO instead of decoding one sample at a time.
   */
  if (copy_bytes > free_bytes){
    copy_bytes = free_bytes;
  }
  copy_bytes &= ~(AUDIO_SAMPLE_BYTES - 1);
  if (copy_bytes == 0){
    return 0;
  }

  /*
   * If the empty ring starts on a different 4-byte phase than the PCM payload,
   * insert one silent 16-bit sample so source and destination stay in the same
   * mod-4 class for the rest of the stream. That adds only 40 microseconds of
   * silence at 25 kHz but unlocks the aligned 32-bit copy path.
   */
  if (audio_ring_ptr_mod4(write_idx) != (((unsigned)src) & (AUDIO_WORD_BYTES - 1)) &&
      free_bytes >= AUDIO_SAMPLE_BYTES){
    write_idx = audio_write_silence_sample(write_idx);
    free_bytes -= AUDIO_SAMPLE_BYTES;
    if (copy_bytes > free_bytes){
      copy_bytes = free_bytes;
    }
    copy_bytes &= ~(AUDIO_SAMPLE_BYTES - 1);
    if (copy_bytes == 0){
      *AUDIO_WRITE_IDX = write_idx;
      return 0;
    }
  }

  /*
   * If source and destination are both 2 mod 4, peel one real sample so the
   * remaining pointers both become 4-byte aligned before the bulk copy.
   */
  if ((((unsigned)src) & (AUDIO_WORD_BYTES - 1)) != 0 &&
      copy_bytes >= AUDIO_SAMPLE_BYTES){
    write_idx = audio_copy_even_bytes_to_ring_asm(src, write_idx,
      AUDIO_SAMPLE_BYTES);
    src += AUDIO_SAMPLE_BYTES;
    consumed_bytes += AUDIO_SAMPLE_BYTES;
    copy_bytes -= AUDIO_SAMPLE_BYTES;
  }

  word_copy_bytes = copy_bytes & ~(AUDIO_WORD_BYTES - 1);
  if (word_copy_bytes != 0){
    write_idx = audio_copy_word_bytes_to_ring_asm(src, write_idx,
      word_copy_bytes);
    src += word_copy_bytes;
    consumed_bytes += word_copy_bytes;
  }

  tail_copy_bytes = copy_bytes - word_copy_bytes;
  if (tail_copy_bytes != 0){
    write_idx = audio_copy_even_bytes_to_ring_asm(src, write_idx,
      tail_copy_bytes);
    consumed_bytes += tail_copy_bytes;
  }

  *AUDIO_WRITE_IDX = write_idx;
  return consumed_bytes;
}

// Validate and load WAV metadata and sample bytes from an ext2 node.
bool audio_wav_load(struct Node* wav_node, struct AudioWav* wav_out){
  unsigned wav_size;
  char* wav_bytes;
  struct AudioWavError error;

  assert(wav_node != NULL,
    "audio wav load: source Node must not be NULL.\n");
  assert(node_is_file(wav_node),
    "audio wav load: source Node must be a regular file.\n");
  assert(wav_out != NULL,
    "audio wav load: result storage must not be NULL.\n");

  wav_size = node_size_in_bytes(wav_node);
  if (wav_size == 0){
    error.code = AUDIO_WAV_ERROR_FILE_TOO_SMALL;
    error.offset = 0;
    error.got = 0;
    error.expected = WAV_RIFF_HEADER_BYTES;
    audio_wav_report_error(wav_node, wav_size, &error);
    return false;
  }

  wav_bytes = mmap(wav_size, wav_node, 0, MMAP_READ);
  if (wav_bytes == NULL){
    // The admission handshake returns this failure to the still-blocked
    // syscall. Keep the diagnostic as well so VM exhaustion is actionable.
    void* args[2];
    args[0] = (void*)wav_node->cached->inumber;
    args[1] = (void*)wav_size;
    say("audio wav load: operation=mmap inode=%d size=%d failed\n", args);
    return false;
  }

  wav_out->source_inumber = wav_node->cached->inumber;
  if (!audio_wav_parse(wav_bytes, wav_size, wav_out, &error)){
    audio_wav_report_error(wav_node, wav_size, &error);

    // The VME belongs to this worker TCB. Remove it here so a rejected request
    // cannot retain physical pages or a cloned Node until thread reaping.
    munmap(wav_bytes);
    wav_out->bytes = NULL;
    return false;
  }

  return true;
}

// Return the number of complete stereo samples in a loaded WAV.
unsigned audio_wav_num_samples(struct AudioWav* wav){
  return wav->data_size / AUDIO_SAMPLE_BYTES;
}

// Start playback of a loaded WAV if the device can accept its format.
bool audio_wav_play(struct AudioWav* wav){
  assert(wav != NULL && wav->bytes != NULL,
    "audio playback: validated WAV and mapping must not be NULL.\n");
  assert(wav->data_size >= AUDIO_SAMPLE_BYTES &&
      (wav->data_size % AUDIO_SAMPLE_BYTES) == 0,
    "audio playback: validated data must contain complete PCM samples.\n");
  assert(wav->data_offset <= wav->file_size &&
      wav->data_size <= wav->file_size - wav->data_offset,
    "audio playback: validated data range must remain inside its mapping.\n");

  blocking_lock_acquire(&audio_lock);

  unsigned next_data_bytes = 0;
  unsigned filled_bytes;
  bool playback_ok = true;

  audio_output_reset(AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES);
  filled_bytes = audio_output_fill_pcm_s16le(wav->bytes + wav->data_offset,
    wav->data_size);
  next_data_bytes += filled_bytes;
  if (filled_bytes == 0){
    playback_ok = false;
  } else {
    audio_output_enable();
  }

  while (playback_ok && next_data_bytes < wav->data_size){
    unsigned wait_deadline =
      (unsigned)__atomic_load_n((int*)&current_jiffies) +
      AUDIO_PROGRESS_TIMEOUT_JIFFIES;

    /*
     * Wait for LOW_WATER with an elapsed jiffy deadline. Poll one tick at a
     * time so a silent device cannot strand the daemon forever holding
     * audio_lock. AUDIO_PROGRESS_TIMEOUT_JIFFIES is within INT_MAX, so modular
     * half-range ordering matches sleep/SD. The ISR only acknowledges the
     * enabled low-water edge; this polling loop owns playback progress and
     * remains deadline-bounded even if the device never raises that edge.
     */
    while (!audio_output_low_water()){
      unsigned now = (unsigned)__atomic_load_n((int*)&current_jiffies);
      if (audio_deadline_reached(now, wait_deadline)){
        playback_ok = false;
        break;
      }
      sleep(1);
    }

    if (!playback_ok){
      break;
    }

    filled_bytes = audio_output_fill_pcm_s16le(
      wav->bytes + wav->data_offset + next_data_bytes,
      wav->data_size - next_data_bytes);

    if (filled_bytes == 0){
      // A validated source always has at least one complete sample remaining.
      // Zero therefore indicates that the MMIO producer/consumer state cannot
      // accept progress. Abort this asynchronous request without leaking the
      // global audio lock or leaving the device enabled.
      playback_ok = false;
      break;
    }
    next_data_bytes += filled_bytes;
  }

  if (playback_ok){
    unsigned drain_deadline =
      (unsigned)__atomic_load_n((int*)&current_jiffies) +
      AUDIO_PROGRESS_TIMEOUT_JIFFIES;
    while (!audio_output_empty()){
      unsigned now = (unsigned)__atomic_load_n((int*)&current_jiffies);
      if (audio_deadline_reached(now, drain_deadline)){
        playback_ok = false;
        break;
      }
      sleep(1);
    }
  }

  if (!playback_ok){
    void* args[6];
    args[0] = (void*)wav->source_inumber;
    args[1] = (void*)next_data_bytes;
    args[2] = (void*)wav->data_size;
    args[3] = (void*)audio_output_read_idx();
    args[4] = (void*)audio_output_write_idx();
    args[5] = (void*)audio_output_status();
    say("audio playback: operation=refill inode=%d consumed=%d total=%d read_idx=0x%X write_idx=0x%X status=0x%X failed\n",
      args);
  }

  audio_output_disable();

  blocking_lock_release(&audio_lock);
  return playback_ok;
}

/*
 * Audio interrupt handler.
 *
 * Playback progress is deadline-bounded by polling LOW_WATER and the ring
 * indices in audio_wav_play(); no thread is published for an interrupt wake.
 * The device IRQ remains enabled, so this bounded handler must acknowledge
 * each low-water edge without touching scheduler or TCB state.
 *
 * CPU state: kernel ISR context with interrupts disabled by hardware.
 */
void audio_handler(void){
  mark_audio_handled();
}

// Allocate an asynchronous audio request retaining one node reference.
struct AudioRequest* audio_request_create(struct Node* node){
  assert(node != NULL,
    "audio request create: source Node must not be NULL.\n");
  assert(node_is_file(node),
    "audio request create: source Node must be a regular file.\n");

  struct AudioRequest* request = malloc(sizeof(struct AudioRequest));
  if (request == NULL){
    return NULL;
  }

  request->link.next = NULL;
  request->node = node_clone(node);
  if (request->node == NULL){
    free(request);
    return NULL;
  }

  sem_init(&request->ready, 0);
  sem_init(&request->caller_acknowledged, 0);
  __atomic_store_n(&request->validation_succeeded, 0);
  __atomic_store_n(&request->owner_count, 2);
  __atomic_store_n(&request->handoff_finalized, 0);
  return request;
}

/*
 * Enqueue one admission/playback request for the persistent daemon. Returns
 * false when AUDIO_MAX_QUEUED_REQUESTS are already outstanding; the caller
 * must then destroy the unsubmitted request.
 */
bool audio_request_submit(struct AudioRequest* request){
  assert(request != NULL,
    "audio request submit: request must not be NULL.\n");
  assert(audio_daemon_started,
    "audio request submit: persistent daemon must be running.\n");

  /*
   * Reserve with one RMW. Concurrent rejected reservations may transiently
   * raise the counter above the limit, but each such caller rolls its own unit
   * back and never publishes a request or asynchronous-work reference.
   */
  int previous = __atomic_fetch_add(&audio_queued_count, 1);
  if (previous >= AUDIO_MAX_QUEUED_REQUESTS){
    __atomic_fetch_add(&audio_queued_count, -1);
    return false;
  }

  /*
   * Acquire shutdown lifetime before making the request visible. The caller
   * is a normal counted syscall TCB, so event_loop() cannot observe both
   * n_active and the asynchronous-work count as zero during this handoff.
   */
  kernel_async_work_begin();
  generic_spin_queue_add(&audio_request_queue, &request->link);

  struct TCB* daemon = interrupt_waiter_signal(&audio_request_waiter);
  if (daemon != NULL){
    scheduler_wake_thread(daemon);
  }
  return true;
}

// Destroy an audio request that was never submitted to the daemon.
void audio_request_destroy_unsubmitted(struct AudioRequest* request){
  assert(request != NULL,
    "audio request destroy: request must not be NULL.\n");

  node_free(request->node);
  sem_destroy(&request->ready);
  sem_destroy(&request->caller_acknowledged);
  free(request);
}

/*
 * Drop one of the two logical request owners after that owner has completed
 * every semaphore operation and all other request accesses.
 *
 * The architecture memory model is sequentially consistent. Therefore the
 * unique 1->0 transition observes both owners' completed semaphore operations,
 * making sem_destroy()'s external-quiescence precondition true. This function
 * does not free request: the daemon frees the allocation after finalization.
 */
static bool audio_request_release_owner(struct AudioRequest* request){
  int previous_owners = __atomic_fetch_add(&request->owner_count, -1);
  if (previous_owners != 1 && previous_owners != 2){
    void* args[2];
    args[0] = request;
    args[1] = (void*)previous_owners;
    say("audio request release: request=0x%X owners_before=%d invalid\n",
      args);
    panic("audio request release: owner count must be one or two.\n");
  }

  if (previous_owners != 1){
    return false;
  }

  sem_destroy(&request->ready);
  sem_destroy(&request->caller_acknowledged);
  __atomic_store_n(&request->handoff_finalized, 1);
  return true;
}

// Wait until the daemon has accepted or rejected the submitted request.
bool audio_request_wait_until_ready(struct AudioRequest* request){
  assert(request != NULL,
    "audio request wait: request must not be NULL.\n");

  /*
   * This runs in kernel mode on the syscall's current user TCB. sem_down() may
   * context-switch with interrupts enabled in the resumed continuation. The
   * daemon stores validation_succeeded before raising ready, so the
   * sequentially-consistent semaphore/atomic operations publish a complete
   * parse result before this load.
   */
  sem_down(&request->ready);
  bool validation_succeeded =
    __atomic_load_n(&request->validation_succeeded) != 0;

  /*
   * This is the caller's final semaphore operation. The following owner drop
   * occurs only after sem_up() has left its active-operation bookkeeping,
   * which matters when another core schedules the daemon before this core has
   * returned from scheduler_wake_thread(). No caller request access follows
   * audio_request_release_owner().
   */
  sem_up(&request->caller_acknowledged);
  audio_request_release_owner(request);
  return validation_succeeded;
}

// Wait for the daemon to release its final request reference.
static void audio_request_wait_for_finalization(struct AudioRequest* request){
  while (__atomic_load_n(&request->handoff_finalized) == 0){
    yield();
  }
}

// Load and start one queued request, publishing its completion state.
static void audio_process_request(struct AudioRequest* request){
  struct AudioWav wav;

  assert(request != NULL,
    "audio process: request must not be NULL.\n");

  /*
   * The persistent daemon runs in kernel mode with its own address space, so it
   * must create and validate the VME it will itself use. request->node remains
   * live independently of the caller's descriptor.
   */
  bool validation_succeeded = audio_wav_load(request->node, &wav);
  __atomic_store_n(&request->validation_succeeded,
    validation_succeeded ? 1 : 0);
  sem_up(&request->ready);

  sem_down(&request->caller_acknowledged);

  if (validation_succeeded){
    audio_wav_play(&wav);
    munmap(wav.bytes);
  }

  node_free(request->node);

  if (!audio_request_release_owner(request)){
    audio_request_wait_for_finalization(request);
  }

  free(request);
}

/*
 * Post-context-switch publication for an idle audio daemon.
 *
 * Preconditions: the daemon TCB is fully saved, is in no scheduler queue, and
 * current-core interrupts are disabled. If a submitter signalled before this
 * callback published the TCB, the callback consumes that notification and
 * defers exactly one wake without modifying the saved TCB.
 */
static void audio_daemon_block(void* arg){
  struct TCB* daemon = (struct TCB*)arg;
  struct TCB* wakeup = interrupt_waiter_publish(&audio_request_waiter, daemon);
  if (wakeup != NULL){
    scheduler_wake_thread_from_interrupt(wakeup);
  }
}

// Release one daemon-owned request reference and wake its final waiter.
static void audio_request_finish_lifetime(void){
  int previous = __atomic_fetch_add(&audio_queued_count, -1);
  // Do not reject previous > AUDIO_MAX_QUEUED_REQUESTS: a concurrent failed
  // reservation may temporarily own an unpublished unit above the limit.
  if (previous <= 0){
    __atomic_fetch_add(&audio_queued_count, 1);
    int args[1] = {previous};
    say("| audio request finish rejected outstanding=%d\n", args);
    panic("audio request finish: outstanding request count is invalid.\n");
  }

  /*
   * This is the daemon's final action for the request. Capacity is released
   * first so no audio state is touched after the shutdown-lifetime reference
   * reaches zero.
   */
  kernel_async_work_finish();
}

/*
 * Lazily create the persistent daemon's private address space.
 *
 * Preconditions:
 * - kernel mode in audio_daemon's TCB; blocking physmem allocation is allowed
 * - the daemon has removed an accepted request from audio_request_queue
 * - that request still owns one kernel_async_work_count reference
 *
 * The asynchronous-work reference prevents every event loop from entering
 * shutdown if this daemon is preempted or blocks during allocation. Before the
 * first request the daemon therefore owns no finite physical-memory resource
 * that scheduler shutdown could abandon.
 *
 * Postcondition: the current hardware PID and TCB PID identify one valid,
 * initially empty page directory owned by this daemon.
 */
static void audio_daemon_prepare_address_space(struct TCB* daemon){
  assert_always(daemon != NULL && daemon == get_current_tcb(),
    "audio daemon address space: caller must be the current daemon TCB.\n");
  assert_always(daemon->is_daemon,
    "audio daemon address space: current TCB must be persistent.\n");

  if (daemon->pid == 0){
    daemon->pid = create_page_directory();
    assert_always(daemon->pid != 0,
      "audio daemon: failed to allocate a page directory for playback mappings.\n");
    set_pid(daemon->pid);
    tlb_flush();
  }
}

// Service queued audio requests and device low-water interrupts.
static void audio_daemon(void* unused){
  (void)unused;

  // Publish the boot-lifetime TCB before doing any resource-owning work. If no
  // audio request ever arrives, this daemon can be abandoned at shutdown
  // without retaining a finite page-directory allocation.
  struct TCB* me = get_current_tcb();
  __atomic_store_n((int*)&audio_daemon_tcb, (int)me);

  while (true){
    /*
     * Clear a stale notification before checking the real queue state. A
     * submit racing with the check either leaves a request in the queue or a
     * pending waiter event that makes the post-switch callback requeue us.
     */
    interrupt_waiter_prepare(&audio_request_waiter);
    struct GenericQueueElement* element =
      generic_spin_queue_remove(&audio_request_queue);
    if (element == NULL){
      int was = interrupts_disable();
      struct TCB* me = get_current_tcb();
      block(was, audio_daemon_block, me, false);
      continue;
    }

    struct AudioRequest* request = (struct AudioRequest*)element;
    audio_daemon_prepare_address_space(me);
    audio_process_request(request);
    audio_request_finish_lifetime();
  }
}
