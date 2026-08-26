#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include "constants.h"

// Enum of the two SD DMA engines documented in docs/mem_map.md.
enum SdDrive {
  SD_DRIVE_0 = 0,
  SD_DRIVE_1 = 1,
};

/*
 * Software-only errors. Controller errors retain their historical values
 * -1 through -6 from docs/mem_map.md; these values deliberately do not overlap.
 */
#define SD_DRIVER_ERR_INVALID_REQUEST (-100)
#define SD_DRIVER_ERR_TIMEOUT (-101)
#define SD_DRIVER_ERR_UNEXPECTED_STATUS (-102)
#define SD_DRIVER_ERR_QUARANTINED (-103)

// Each drive stages one command through one permanent 4 KiB page. Public calls
// may span more blocks; the command lock serializes them as bounded chunks.
#define SD_DMA_BOUNCE_BLOCKS 8

/*
 * Generation arbitration shared by production code and deterministic tests.
 *
 * Production synchronization:
 * - Every read or mutation is performed while the owning drive's state lock is
 *   held. That lock uses 32-bit sequentially-consistent atomic swap.
 * - `active` identifies the sole generation that the SD ISR or watchdog may
 *   finish. `quarantined` prevents a new generation after a timeout until the
 *   late hardware completion has been acknowledged.
 *
 * Tests may use an independent instance from one thread without a lock. These
 * functions do not access MMIO, allocate, block, or wake a thread.
 */
struct SdRequestState {
  unsigned generation;
  bool active;
  bool quarantined;
  int result;
};

// Initialize an idle, non-quarantined state. Generation zero is reserved.
void sd_request_state_init(struct SdRequestState* state);

/*
 * Begin the next generation and return its nonzero identifier. Return zero if
 * another generation is active or the controller remains quarantined.
 */
unsigned sd_request_state_begin(struct SdRequestState* state);

/*
 * Publish one terminal result for `generation`. Return false without changing
 * state when the generation is stale or already has a terminal publisher.
 */
bool sd_request_state_finish(struct SdRequestState* state,
  unsigned generation, int result, bool quarantine);

/*
 * Clear quarantine only for the matching, already-finished generation. This is
 * the late-IRQ acknowledgement that permits a later request to begin.
 */
bool sd_request_state_acknowledge_quarantine(struct SdRequestState* state,
  unsigned generation);

/*
 * Initialize both controllers and register their handlers.
 *
 * Preconditions: kernel mode on boot core 0; interrupts globally disabled;
 * PIT, thread, and scheduler structures exist, but normal scheduling is not yet
 * live. Failure to initialize either required controller is a fatal boot error
 * with a drive/status/error diagnostic.
 */
void sd_init(void);

/*
 * Destroy SD synchronization during globally quiescent kernel shutdown.
 * No core may issue a new request, own a drive lock, or be waiting for SD.
 */
void sd_destroy(void);

/*
 * Transfer whole 512-byte blocks between one SD drive and physical memory.
 *
 * Preconditions checked by the driver: drive is 0 or 1; start_block is
 * nonnegative; num_blocks is positive; the buffer is non-NULL and 4-byte
 * aligned; block and byte-range arithmetic is representable; and the complete
 * source/destination span lies in ordinary physical RAM below the MMIO window.
 * The wrapper deliberately does not accept MMIO buffers: it stages every
 * command through driver-owned storage so a timed-out, non-atomic DMA cannot
 * retain access to caller-owned memory after this function returns.
 *
 * One request per drive is serialized, while the two drives remain independent.
 * Returns 0, a negative controller error (-1 through -6), or a named software
 * error above. A timeout quarantines only the affected drive and retains that
 * drive's bounce page until a terminal late interrupt is acknowledged.
 */
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks,
  void* dest);
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks,
  void* src);

// Atomically acknowledge the corresponding SD interrupt-status bit.
extern void mark_sd0_handled(void);
extern void mark_sd1_handled(void);

// Assembly ISR wrappers preserve caller-saved registers and return with rfe.
extern void sd0_handler_(void);
extern void sd1_handler_(void);

// C interrupt body called by the wrappers in kernel/sd_driver.s.
void sd_handler(enum SdDrive drive);

// Wrapping half-range deadline predicate shared with focused validation.
// `now` and `deadline` must differ by less than 2^31 ticks.
bool sd_runtime_deadline_reached(unsigned now, unsigned deadline);

#endif // SD_DRIVER_H
