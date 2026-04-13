#include "audio.h"

#include "debug.h"
#include "ext.h"
#include "print.h"
#include "threads.h"
#include "vmem.h"
#include "ivt.h"
#include "blocking_lock.h"
#include "scheduler.h"
#include "interrupts.h"
#include "per_core.h"

struct BlockingLock audio_lock;

struct TCB* audio_wait_thread = NULL;
struct SpinLock audio_wait_thread_lock;

static char* AUDIO_RING = (char*)AUDIO_RING_BASE;
static unsigned* AUDIO_CTRL = (unsigned*)AUDIO_CTRL_ADDR;
static unsigned* AUDIO_STATUS = (unsigned*)AUDIO_STATUS_ADDR;
static unsigned* AUDIO_WRITE_IDX = (unsigned*)AUDIO_WRITE_IDX_ADDR;
static unsigned* AUDIO_READ_IDX = (unsigned*)AUDIO_READ_IDX_ADDR;
static unsigned* AUDIO_WATERMARK = (unsigned*)AUDIO_WATERMARK_ADDR;

#define WAV_RIFF_HEADER_BYTES 12
#define WAV_CHUNK_HEADER_BYTES 8
#define WAV_FMT_MIN_BYTES 16
#define WAV_PCM_FORMAT 1
#define WAV_EXPECTED_CHANNELS 1
#define WAV_EXPECTED_SAMPLE_RATE 25000
#define WAV_EXPECTED_BITS_PER_SAMPLE 16
#define WAV_EXPECTED_BLOCK_ALIGN 2
#define WAV_EXPECTED_BYTE_RATE 50000

// register audio isr and initialize control regs
void audio_init(void){
  register_handler(audio_handler_, (void*)AUDIO_IVT_ENTRY);
  audio_output_reset(AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES);
  blocking_lock_init(&audio_lock);
  spin_lock_init(&audio_wait_thread_lock);
}

// Copy an even number of aligned PCM bytes into the fixed ring with ld/sd.
extern unsigned audio_copy_even_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

// Copy a multiple of 4 aligned PCM bytes into the fixed ring with lw/sw.
extern unsigned audio_copy_word_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

static unsigned read_u16_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8);
}

static unsigned read_u32_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8) |
         ((unsigned)(unsigned char)bytes[2] << 16) |
         ((unsigned)(unsigned char)bytes[3] << 24);
}

static bool chunk_id_is(char* bytes, char a, char b, char c, char d){
  return bytes[0] == a && bytes[1] == b && bytes[2] == c && bytes[3] == d;
}

static void panic_wav_value_mismatch(char* field,
    unsigned got, unsigned expected){
  void* args[3];

  args[0] = field;
  args[1] = (void*)got;
  args[2] = (void*)expected;
  say("audio wav parse: field=%s got=0x%X expected=0x%X\n", args);
  panic("audio wav parse: unsupported wav format\n");
}

static void audio_wav_parse(char* wav,
    unsigned wav_size, struct AudioWav* wav_out){
  bool saw_fmt = false;
  unsigned offset = WAV_RIFF_HEADER_BYTES;

  if (wav_size < WAV_RIFF_HEADER_BYTES){
    panic("wav is too small for the RIFF header");
  }
  if (!chunk_id_is(wav, 'R', 'I', 'F', 'F')){
    panic("wav is missing the RIFF signature");
  }
  if (!chunk_id_is(wav + 8, 'W', 'A', 'V', 'E')){
    panic("wav is missing the WAVE signature");
  }
  if (read_u32_le(wav + 4) > wav_size - 8){
    panic("wav has a RIFF size that exceeds the mapped file");
  }

  wav_out->bytes = wav;
  wav_out->file_size = wav_size;
  wav_out->data_offset = 0;
  wav_out->data_size = 0;

  while (offset + WAV_CHUNK_HEADER_BYTES <= wav_size){
    char* chunk = wav + offset;
    unsigned chunk_size = read_u32_le(chunk + 4);
    unsigned chunk_data_offset = offset + WAV_CHUNK_HEADER_BYTES;
    unsigned padded_chunk_size = chunk_size + (chunk_size & 1);

    if (chunk_size > wav_size - chunk_data_offset){
      panic("wav contains a chunk that extends past end of file");
    }
    if (padded_chunk_size > wav_size - chunk_data_offset){
      panic("wav contains chunk padding that extends past end of file");
    }

    if (chunk_id_is(chunk, 'f', 'm', 't', ' ')){
      unsigned audio_format;
      unsigned num_channels;
      unsigned sample_rate;
      unsigned byte_rate;
      unsigned block_align;
      unsigned bits_per_sample;

      if (chunk_size < WAV_FMT_MIN_BYTES){
        panic("wav has a fmt chunk that is too short");
      }

      audio_format = read_u16_le(chunk + 8);
      num_channels = read_u16_le(chunk + 10);
      sample_rate = read_u32_le(chunk + 12);
      byte_rate = read_u32_le(chunk + 16);
      block_align = read_u16_le(chunk + 20);
      bits_per_sample = read_u16_le(chunk + 22);

      if (audio_format != WAV_PCM_FORMAT){
        panic_wav_value_mismatch("audio_format", audio_format,
          WAV_PCM_FORMAT);
      }
      if (num_channels != WAV_EXPECTED_CHANNELS){
        panic_wav_value_mismatch("num_channels", num_channels,
          WAV_EXPECTED_CHANNELS);
      }
      if (sample_rate != WAV_EXPECTED_SAMPLE_RATE){
        panic_wav_value_mismatch("sample_rate", sample_rate,
          WAV_EXPECTED_SAMPLE_RATE);
      }
      if (bits_per_sample != WAV_EXPECTED_BITS_PER_SAMPLE){
        panic_wav_value_mismatch("bits_per_sample", bits_per_sample,
          WAV_EXPECTED_BITS_PER_SAMPLE);
      }
      if (block_align != WAV_EXPECTED_BLOCK_ALIGN){
        panic_wav_value_mismatch("block_align", block_align,
          WAV_EXPECTED_BLOCK_ALIGN);
      }
      if (byte_rate != WAV_EXPECTED_BYTE_RATE){
        panic_wav_value_mismatch("byte_rate", byte_rate,
          WAV_EXPECTED_BYTE_RATE);
      }

      saw_fmt = true;
    } else if (chunk_id_is(chunk, 'd', 'a', 't', 'a')){
      if (!saw_fmt){
        panic("wav contains a data chunk before fmt");
      }
      wav_out->data_offset = chunk_data_offset;
      wav_out->data_size = chunk_size;
      return;
    }

    offset = chunk_data_offset + padded_chunk_size;
  }

  panic("wav is missing a data chunk");
}

unsigned audio_output_buffered_bytes(unsigned write_idx, unsigned read_idx){
  if (write_idx >= read_idx){
    return write_idx - read_idx;
  }
  return AUDIO_RING_SIZE_BYTES - (read_idx - write_idx);
}

unsigned audio_output_free_bytes(unsigned write_idx, unsigned read_idx){
  return AUDIO_USABLE_BYTES - audio_output_buffered_bytes(write_idx, read_idx);
}

unsigned audio_output_advance_idx(unsigned idx, unsigned bytes){
  idx += bytes;
  if (idx >= AUDIO_RING_SIZE_BYTES){
    idx -= AUDIO_RING_SIZE_BYTES;
  }
  return idx;
}

void audio_output_write_sample_s16le(int sample, unsigned write_idx){
  unsigned encoded = (unsigned short)sample;
  unsigned next_idx = audio_output_advance_idx(write_idx, 1);

  AUDIO_RING[write_idx] = encoded & 0xFF;
  AUDIO_RING[next_idx] = (encoded >> 8) & 0xFF;
}

unsigned audio_output_status(void){
  return *AUDIO_STATUS;
}

unsigned audio_output_write_idx(void){
  return *AUDIO_WRITE_IDX;
}

unsigned audio_output_read_idx(void){
  return *AUDIO_READ_IDX;
}

void audio_output_set_write_idx(unsigned write_idx){
  *AUDIO_WRITE_IDX = write_idx;
}

void audio_output_reset(unsigned watermark_bytes){
  unsigned read_idx = *AUDIO_READ_IDX;

  *AUDIO_CTRL = 0;
  *AUDIO_WATERMARK = watermark_bytes;
  *AUDIO_WRITE_IDX = read_idx;
}

void audio_output_enable(void){
  *AUDIO_CTRL = AUDIO_CTRL_ENABLE | AUDIO_CTRL_IRQ;
}

void audio_output_disable(void){
  *AUDIO_CTRL = 0;
}

bool audio_output_low_water(void){
  return ((*AUDIO_STATUS) & AUDIO_STATUS_LOW_WATER) != 0;
}

bool audio_output_empty(void){
  return *AUDIO_READ_IDX == *AUDIO_WRITE_IDX;
}

static unsigned audio_ring_ptr_mod4(unsigned write_idx){
  return (AUDIO_RING_BASE + write_idx) & (AUDIO_WORD_BYTES - 1);
}

static unsigned audio_write_silence_sample(unsigned write_idx){
  AUDIO_RING[write_idx] = 0;
  AUDIO_RING[audio_output_advance_idx(write_idx, 1)] = 0;
  return audio_output_advance_idx(write_idx, AUDIO_SAMPLE_BYTES);
}

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

void audio_wav_load(struct Node* wav_node, struct AudioWav* wav_out){
  unsigned wav_size;
  char* wav_bytes;
  void* args[1];

  wav_size = node_size_in_bytes(wav_node);
  wav_bytes = mmap(wav_size, wav_node, 0, MMAP_READ);

  audio_wav_parse(wav_bytes, wav_size, wav_out);
}

unsigned audio_wav_num_samples(struct AudioWav* wav){
  return wav->data_size / AUDIO_SAMPLE_BYTES;
}

void audio_block_thread(void* arg){
  int* args = (int*)arg;
  struct TCB* tcb = (struct TCB*)args[0];
  audio_wait_thread = tcb;

  spin_lock_release(&audio_wait_thread_lock);
}

void audio_wav_play(struct AudioWav* wav){
  blocking_lock_acquire(&audio_lock);

  unsigned next_data_bytes = 0;
  unsigned filled_bytes;

  audio_output_reset(AUDIO_OUTPUT_DEFAULT_WATERMARK_BYTES);
  filled_bytes = audio_output_fill_pcm_s16le(wav->bytes + wav->data_offset,
    wav->data_size);
  next_data_bytes += filled_bytes;
  audio_output_enable();

  while (next_data_bytes < wav->data_size){

    // TODO: ensure O(1) critical sections
    // for now im okay with this because there shouldnt be much contention
    // on this lock
    int was = interrupts_disable();
    spin_lock_acquire(&audio_wait_thread_lock);
    if (!audio_output_low_water()){
      struct TCB* current_tcb = get_current_tcb();
      int args[1] = { (int)current_tcb };
      block(was, audio_block_thread, (void*)(args), false);
    } else {
      spin_lock_release(&audio_wait_thread_lock);
      interrupts_restore(was);
    }

    filled_bytes = audio_output_fill_pcm_s16le(
      wav->bytes + wav->data_offset + next_data_bytes,
      wav->data_size - next_data_bytes);

    next_data_bytes += filled_bytes;
  }

  while (!audio_output_empty()){
    sleep(1);
  }

  audio_output_disable();

  blocking_lock_release(&audio_lock);
}

void audio_handler(void){
  mark_audio_handled();

  spin_lock_acquire(&audio_wait_thread_lock);

  struct TCB* tcb = audio_wait_thread;
  audio_wait_thread = NULL;

  spin_lock_release(&audio_wait_thread_lock);

  if (tcb != NULL){
    scheduler_wake_thread_from_interrupt(tcb);
  }
}
