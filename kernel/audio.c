#include "audio.h"

#include "debug.h"
#include "print.h"
#include "threads.h"
#include "ivt.h"
#include "heap.h"
#include "interrupts.h"
#include "atomic.h"
#include "sys.h"
#include "syscall_errors.h"
#include "TCB.h"
#include "per_core.h"

/*
 * Audio output driver: one process at a time owns the MMIO PCM ring and feeds
 * it with non-blocking writes (see audio.h for the user-visible contract).
 *
 * Ownership is the synchronization. Invariants:
 * - audio_owner changes only from NULL to a TCB (handle_audio_open, under
 *   audio_open_lock) or from a TCB back to NULL (handle_audio_close or
 *   audio_release_owner, both running in that same TCB's context).
 * - Only the owner touches the producer side of the device (AUDIO_CTRL,
 *   AUDIO_WRITE_IDX, AUDIO_WATERMARK, ring bytes), audio_playing, and the
 *   staging buffer.
 *
 * Consequences:
 * - Once a syscall sees audio_owner == current TCB, ownership cannot change
 *   until that TCB itself releases it. User processes are single-threaded and
 *   signal handlers run only at syscall return, so the owner never runs two
 *   audio operations at once, and these paths need no lock. That includes
 *   copy_from_user(), which may block on demand paging.
 * - Release is "stop the device, then store NULL". It never blocks or spins,
 *   so stop() can run it even on the signal-termination path, which has
 *   interrupts disabled. The next owner's open happens only after it sees
 *   NULL, so it can never overlap the previous owner's device accesses.
 * - audio_open_lock makes check-then-set atomic among competing openers. The
 *   compiler offers no compare-and-swap builtin, so this is a spinlock held
 *   for two memory operations.
 *
 * The device advances AUDIO_READ_IDX concurrently. A snapshot of it is
 * conservative for the producer: consumption only frees space, so space
 * computed from an older snapshot is still available. The memory model is
 * sequentially consistent (docs/ISA.md); the atomic accessors make the
 * cross-thread accesses to audio_owner explicit.
 */

static struct SpinLock audio_open_lock;
static struct TCB* audio_owner;
// Whether the owner has enabled playback since its open. Owner-only.
static bool audio_playing;

/*
 * Kernel staging buffer for user PCM. The user copy lands at offset
 * (write_idx & 3) so the staged bytes have the same mod-4 phase as their ring
 * destination, which lets audio_copy_to_ring() use word copies for the bulk of
 * every write regardless of the user buffer's alignment.
 */
static char* audio_staging_alloc;
static char* audio_staging;
#define AUDIO_STAGING_BYTES (AUDIO_USABLE_BYTES + AUDIO_WORD_BYTES)

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

// Copy an even number of aligned PCM bytes into the fixed ring with ld/sd.
extern unsigned audio_copy_even_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

// Copy a multiple of 4 aligned PCM bytes into the fixed ring with lw/sw.
extern unsigned audio_copy_word_bytes_to_ring_asm(char* src, unsigned write_idx,
    unsigned copy_bytes);

// Register the audio ISR and leave the device stopped with no owner.
void audio_init(void){
  spin_lock_init(&audio_open_lock);
  audio_owner = NULL;
  audio_playing = false;

  // Round the staging buffer up to a word boundary so its phase arithmetic
  // matches the 4-byte-aligned ring base.
  audio_staging_alloc = malloc(AUDIO_STAGING_BYTES + AUDIO_WORD_BYTES);
  audio_staging = (char*)(((unsigned)audio_staging_alloc + AUDIO_WORD_BYTES - 1) &
    ~(AUDIO_WORD_BYTES - 1));

  // The driver never sets IRQ_EN, but keep a handler installed so a stray
  // audio interrupt is acknowledged instead of hitting an empty IVT slot.
  register_handler(audio_handler_, (void*)AUDIO_IVT_ENTRY);
  audio_output_reset(0);
}

// to be called only from kernel_shutdown
void audio_destroy(void){
  /*
   * Every core is past the shutdown barrier, so n_active reached zero and
   * every user process has run stop(), which releases audio ownership before
   * publishing its exit. A surviving owner means a termination path skipped
   * audio_release_owner().
   */
  struct TCB* owner = (struct TCB*)__atomic_load_n((int*)&audio_owner);
  if (owner != NULL){
    int args[1] = {(int)owner};
    say("| audio destroy: device still owned by tcb=0x%X\n", args);
    panic("audio destroy: an exited process kept audio ownership; every exit path must call audio_release_owner().\n");
  }

  audio_output_disable();
  free(audio_staging_alloc);
  audio_staging_alloc = NULL;
  audio_staging = NULL;
}

/*
 * Copy `bytes` (even) of staged PCM into the ring starting at `write_idx` and
 * return the new write index. Does not publish AUDIO_WRITE_IDX.
 *
 * Precondition: `src` has the same address mod 4 as the ring destination
 * (AUDIO_RING_BASE + write_idx); both are then either word-aligned or both
 * 2 mod 4, and one peeled sample aligns them for the word copy.
 */
static unsigned audio_copy_to_ring(char* src, unsigned write_idx, unsigned bytes){
  if ((((unsigned)src) & (AUDIO_WORD_BYTES - 1)) != 0 && bytes >= AUDIO_SAMPLE_BYTES){
    write_idx = audio_copy_even_bytes_to_ring_asm(src, write_idx, AUDIO_SAMPLE_BYTES);
    src += AUDIO_SAMPLE_BYTES;
    bytes -= AUDIO_SAMPLE_BYTES;
  }

  unsigned word_bytes = bytes & ~(AUDIO_WORD_BYTES - 1);
  if (word_bytes != 0){
    write_idx = audio_copy_word_bytes_to_ring_asm(src, write_idx, word_bytes);
    src += word_bytes;
  }

  unsigned tail_bytes = bytes - word_bytes;
  if (tail_bytes != 0){
    write_idx = audio_copy_even_bytes_to_ring_asm(src, write_idx, tail_bytes);
  }
  return write_idx;
}

// Stop playback and empty the ring. Caller is the owner (or is claiming
// ownership).
static void audio_stop(void){
  audio_output_reset(0);
  audio_playing = false;
}

// Claim the device for the calling process and reset it to empty and stopped.
int handle_audio_open(void){
  struct TCB* tcb = get_current_tcb();

  spin_lock_acquire(&audio_open_lock);
  bool claimed = __atomic_load_n((int*)&audio_owner) == (int)NULL;
  if (claimed){
    __atomic_store_n((int*)&audio_owner, (int)tcb);
  }
  spin_lock_release(&audio_open_lock);

  if (!claimed){
    set_syscall_error(EBUSY);
    return -1;
  }
  // The previous owner stopped the device before releasing it; reset again
  // so a fresh owner never inherits a watermark or index left by kernel-only
  // users of the low-level helpers.
  audio_stop();
  return 0;
}

/*
 * Copy as much of the caller's PCM as currently fits into the ring and start
 * playback if it was stopped.
 *
 * The accepted count is min(bytes, free ring space) and is always even. Only
 * that prefix of `pcm` is validated and read; bytes the ring cannot take yet
 * are left for the caller to resubmit.
 */
int handle_audio_write(char* pcm, unsigned bytes){
  struct TCB* tcb = get_current_tcb();

  if ((struct TCB*)__atomic_load_n((int*)&audio_owner) != tcb){
    set_syscall_error(EBADF);
    return -1;
  }
  if ((bytes & (AUDIO_SAMPLE_BYTES - 1)) != 0){
    set_syscall_error(EINVAL);
    return -1;
  }

  unsigned write_idx = *AUDIO_WRITE_IDX;
  unsigned free_bytes = audio_output_free_bytes(write_idx, *AUDIO_READ_IDX);
  unsigned accept_bytes = bytes < free_bytes ? bytes : free_bytes;
  if (accept_bytes == 0){
    return 0;
  }

  char* staged = audio_staging + (write_idx & (AUDIO_WORD_BYTES - 1));
  if (copy_from_user(staged, pcm, accept_bytes, tcb) != 0){
    set_syscall_error(EFAULT);
    return -1;
  }

  // Ring bytes first, then the producer index, so the device never sees an
  // index covering bytes that are not written yet.
  *AUDIO_WRITE_IDX = audio_copy_to_ring(staged, write_idx, accept_bytes);
  if (!audio_playing){
    audio_output_enable();
    audio_playing = true;
  }
  return accept_bytes;
}

// Report how many accepted bytes the device has not played yet.
int handle_audio_buffered(void){
  struct TCB* tcb = get_current_tcb();

  if ((struct TCB*)__atomic_load_n((int*)&audio_owner) != tcb){
    set_syscall_error(EBADF);
    return -1;
  }
  return audio_output_buffered_bytes(*AUDIO_WRITE_IDX, *AUDIO_READ_IDX);
}

// Stop playback, discard queued audio, and release the caller's ownership.
int handle_audio_close(void){
  struct TCB* tcb = get_current_tcb();

  if ((struct TCB*)__atomic_load_n((int*)&audio_owner) != tcb){
    set_syscall_error(EBADF);
    return -1;
  }
  // Stop before publishing NULL so a new owner never overlaps our accesses.
  audio_stop();
  __atomic_store_n((int*)&audio_owner, (int)NULL);
  return 0;
}

/*
 * Release the device if `tcb` owns it.
 *
 * Runs on every thread termination (from stop()) and on every successful exec,
 * in `tcb`'s own context, so ownership by `tcb` cannot change during the
 * check. Never blocks or spins, so it is safe with interrupts disabled.
 */
void audio_release_owner(struct TCB* tcb){
  if ((struct TCB*)__atomic_load_n((int*)&audio_owner) != tcb){
    return;
  }
  audio_stop();
  __atomic_store_n((int*)&audio_owner, (int)NULL);
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

// Read the producer index published by software.
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

// Enable audio playback after the ring has been prepared. IRQ_EN stays clear.
void audio_output_enable(void){
  *AUDIO_CTRL = AUDIO_CTRL_ENABLE;
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

/*
 * Audio interrupt handler. The driver never enables the LOW_WATER interrupt,
 * so this only acknowledges a stray one.
 *
 * CPU state: kernel ISR context with interrupts disabled by hardware.
 */
void audio_handler(void){
  mark_audio_handled();
}
