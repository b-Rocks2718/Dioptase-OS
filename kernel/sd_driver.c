#include "sd_driver.h"

#include "atomic.h"
#include "blocking_lock.h"
#include "debug.h"
#include "heap.h"
#include "interrupt_waiter.h"
#include "interrupts.h"
#include "ivt.h"
#include "per_core.h"
#include "pit.h"
#include "print.h"
#include "scheduler.h"
#include "string.h"
#include "threads.h"

/* SD DMA register addresses from docs/mem_map.md. */
#define SD0_DMA_MEM_ADDR 0x07FE5810
#define SD0_DMA_BLOCK_ADDR 0x07FE5814
#define SD0_DMA_LEN_ADDR 0x07FE5818
#define SD0_DMA_CTRL_ADDR 0x07FE581C
#define SD0_DMA_STATUS_ADDR 0x07FE5820
#define SD0_DMA_ERR_ADDR 0x07FE5824

#define SD1_DMA_MEM_ADDR 0x07FE5828
#define SD1_DMA_BLOCK_ADDR 0x07FE582C
#define SD1_DMA_LEN_ADDR 0x07FE5830
#define SD1_DMA_CTRL_ADDR 0x07FE5834
#define SD1_DMA_STATUS_ADDR 0x07FE5838
#define SD1_DMA_ERR_ADDR 0x07FE583C

#define SD_BLOCK_SIZE_BYTES 512
#define SD_DMA_ALIGNMENT_BYTES 4
// docs/kernel_mem_map.md: this is the first MMIO address after ordinary RAM.
#define SD_DMA_RAM_END_EXCLUSIVE 0x07FB8000
#define SD_DMA_BOUNCE_BYTES 4096

#define SD_DMA_CTRL_START 0x1
#define SD_DMA_CTRL_DIR_RAM_TO_SD 0x2
#define SD_DMA_CTRL_IRQ_ENABLE 0x4
#define SD_DMA_CTRL_SD_INIT 0x8

#define SD_DMA_STATUS_BUSY 0x1
#define SD_DMA_STATUS_DONE 0x2
#define SD_DMA_STATUS_ERR 0x4
#define SD_DMA_STATUS_KNOWN_MASK 0x7

#define SD_CONTROLLER_ERR_BUSY 1
#define SD_CONTROLLER_ERR_MAX 6

/*
 * The SD hardware specifies its own command-level timeouts but does not specify
 * a software wait deadline. These two bounds are therefore implementation-
 * defined kernel policy:
 *
 * - Runtime commands get 30,000 PIT ticks. kernel_entry programs the PIT for
 *   3,000 Hz, so the normal configuration permits roughly ten seconds. The
 *   value is well below INT_MAX, preserving unambiguous wrapping-jiffy order.
 * - Boot has no live scheduler/PIT wake path. It performs at most 16,777,216
 *   status reads. This is deliberately an operation bound, not a time claim;
 *   the power-of-two value is an explicit policy window and makes no claim
 *   about a hardware duration the specification does not define.
 */
#define SD_RUNTIME_TIMEOUT_JIFFIES 30000
#define SD_WATCHDOG_POLL_JIFFIES 30
#define SD_BOOT_POLL_OPERATION_LIMIT 16777216

// Identify the SD commands issued by the filesystem and boot paths.
enum SdOperation {
  SD_OPERATION_INIT = 0,
  SD_OPERATION_READ = 1,
  SD_OPERATION_WRITE = 2,
};

/*
 * Raw state lock usable by both thread and interrupt context.
 *
 * This cannot use SpinLock/CLHLock because those primitives consume TCB lock
 * ownership state and may assert if an ISR interrupted code that owned another
 * spinlock. Every thread-context attempt masks all interrupts before the swap,
 * so an SD ISR cannot interrupt a same-core owner. A cross-core owner executes
 * only the bounded, allocation-free state/MMIO updates documented below; it
 * never blocks or acquires another lock. Consequently an ISR may spin here
 * without creating a lock cycle.
 */
struct SdStateLock {
  int held;
};

// Track one SD controller's request generation, deadline, and quarantine state.
struct SdDriveContext {
  struct BlockingLock command_lock;
  struct SdStateLock state_lock;
  struct InterruptWaiter waiter;
  struct SdRequestState request;

  unsigned deadline_jiffies;
  enum SdOperation operation;
  unsigned start_block;
  unsigned num_blocks;
  unsigned buffer_addr;
  unsigned command;

  unsigned last_status;
  unsigned last_error;

  /*
   * True after the watchdog publishes a terminal result without clearing the
   * controller's sticky state. The hardware IRQ for that completed command may
   * already be routed but delayed on another core. The next handler consumes
   * this provenance even if a newer command has started; terminal status then
   * belongs to the active command, while nonterminal status identifies the
   * handler as the delayed edge and must not finish the newer generation.
   *
   * All access is under state_lock. Commands are serialized per drive and the
   * interrupt source is a pending bit rather than a counted event, so one bit
   * records all unconsumed terminal-watchdog completions safely.
   */
  bool late_terminal_irq_pending;

  // One page permanently owned by this drive. Hardware never receives a
  // caller address. Quarantine prevents restaging while an old generation may
  // still be performing non-atomic DMA into/from this page.
  char* bounce_buffer;
};

static struct SdDriveContext sd_contexts[2];

// Return the MMIO memory-address register for one SD controller.
// The cell is volatile so each store reaches the controller.
static volatile unsigned* sd_mem_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (volatile unsigned*)SD0_DMA_MEM_ADDR;
  return (volatile unsigned*)SD1_DMA_MEM_ADDR;
}

// Return the MMIO block-number register for one SD controller.
static volatile unsigned* sd_block_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (volatile unsigned*)SD0_DMA_BLOCK_ADDR;
  return (volatile unsigned*)SD1_DMA_BLOCK_ADDR;
}

// Return the MMIO transfer-length register for one SD controller.
static volatile unsigned* sd_len_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (volatile unsigned*)SD0_DMA_LEN_ADDR;
  return (volatile unsigned*)SD1_DMA_LEN_ADDR;
}

// Return the MMIO command/control register for one SD controller.
static volatile unsigned* sd_ctrl_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (volatile unsigned*)SD0_DMA_CTRL_ADDR;
  return (volatile unsigned*)SD1_DMA_CTRL_ADDR;
}

// Return the MMIO status register for one SD controller.
// Software writes this register to clear sticky DONE and ERR bits.
static volatile unsigned* sd_status_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (volatile unsigned*)SD0_DMA_STATUS_ADDR;
  return (volatile unsigned*)SD1_DMA_STATUS_ADDR;
}

// Return the read-only MMIO error register for one SD controller.
static const volatile unsigned* sd_error_reg(enum SdDrive drive){
  if (drive == SD_DRIVE_0) return (const volatile unsigned*)SD0_DMA_ERR_ADDR;
  return (const volatile unsigned*)SD1_DMA_ERR_ADDR;
}

// Return whether drive selects one of the implemented SD controllers.
static bool sd_drive_is_valid(enum SdDrive drive){
  return drive == SD_DRIVE_0 || drive == SD_DRIVE_1;
}

// Map an SD operation code to its diagnostic name.
static char* sd_operation_name(enum SdOperation operation){
  if (operation == SD_OPERATION_INIT) return "init";
  if (operation == SD_OPERATION_READ) return "read";
  if (operation == SD_OPERATION_WRITE) return "write";
  return "invalid-operation";
}

// Initialize request state before the first controller generation.
void sd_request_state_init(struct SdRequestState* state){
  assert(state != NULL,
    "sd request state init: state pointer is NULL.\n");
  state->generation = 0;
  state->active = false;
  state->quarantined = false;
  state->result = 0;
}

// Begin a new request generation unless the state is active or quarantined.
unsigned sd_request_state_begin(struct SdRequestState* state){
  assert(state != NULL,
    "sd request state begin: state pointer is NULL.\n");

  if (state->active || state->quarantined){
    return 0;
  }

  unsigned next_generation = state->generation + 1;
  if (next_generation == 0){
    // Zero is the begin-failure sentinel, so skip it after 32-bit wrap.
    next_generation = 1;
  }

  state->generation = next_generation;
  state->active = true;
  state->result = SD_DRIVER_ERR_UNEXPECTED_STATUS;
  return next_generation;
}

// Publish a request's terminal result and wake its waiter.
bool sd_request_state_finish(struct SdRequestState* state,
    unsigned generation, int result, bool quarantine){
  assert(state != NULL,
    "sd request state finish: state pointer is NULL.\n");

  if (generation == 0 || !state->active ||
      state->generation != generation){
    return false;
  }

  state->result = result;
  state->quarantined = quarantine;
  state->active = false;
  return true;
}

// Finish controller state while preserving generation/quarantine ordering.
bool sd_request_state_finish_controller(struct SdRequestState* state,
    unsigned generation, int result, bool controller_busy){
  /*
   * BUSY is the hardware ownership boundary. DONE/ERR alone is sticky status,
   * not evidence that DMA can still access the drive's bounce page.
   */
  return sd_request_state_finish(state, generation, result, controller_busy);
}

// Acknowledge a timed-out controller and permit a new generation.
bool sd_request_state_acknowledge_quarantine(struct SdRequestState* state,
    unsigned generation){
  assert(state != NULL,
    "sd request state acknowledge: state pointer is NULL.\n");

  if (generation == 0 || state->active || !state->quarantined ||
      state->generation != generation){
    return false;
  }

  state->quarantined = false;
  return true;
}

/*
 * Acquire one drive's state lock with the complete IMR masked.
 *
 * Preconditions: kernel mode; lock initialized; caller does not already own
 * this state lock. The caller may be an SD ISR, whose IMR global-enable bit is
 * already clear. Postcondition: the lock is held and the complete IMR is zero;
 * the returned mask must be passed exactly once to sd_state_lock_release().
 */
static unsigned sd_state_lock_acquire(struct SdStateLock* lock){
  unsigned was = interrupts_disable();
  while (__atomic_exchange_n(&lock->held, true)){
    // The remote owner executes a bounded nonblocking critical section.
  }
  return was;
}

/*
 * Release the state lock and restore the caller's exact IMR. Atomic store is
 * sufficient under the architecture's sequentially-consistent memory model:
 * all state/MMIO reads and writes become visible before a later acquirer.
 */
static void sd_state_lock_release(struct SdStateLock* lock, unsigned was){
  __atomic_store_n(&lock->held, false);
  interrupts_restore(was);
}

// Clear the controller's latched status and error registers.
static void sd_clear_status(enum SdDrive drive){
  // Per docs/mem_map.md, any status write clears DONE, ERR, and DMA_ERR only.
  *sd_status_reg(drive) = 0;
}

// Return whether status/error indicate a completed SD request.
static bool sd_status_is_terminal(unsigned status, unsigned error){
  return (status & (SD_DMA_STATUS_DONE | SD_DMA_STATUS_ERR)) != 0 ||
    error != 0;
}

// Translate terminal SD status and error registers to an OS result code.
static int sd_result_from_status(unsigned status, unsigned error){
  if ((status & ~SD_DMA_STATUS_KNOWN_MASK) != 0){
    return SD_DRIVER_ERR_UNEXPECTED_STATUS;
  }

  if (error != 0){
    if (error <= SD_CONTROLLER_ERR_MAX){
      return 0 - (int)error;
    }
    return SD_DRIVER_ERR_UNEXPECTED_STATUS;
  }

  if ((status & SD_DMA_STATUS_BUSY) != 0 ||
      (status & SD_DMA_STATUS_ERR) != 0 ||
      (status & SD_DMA_STATUS_DONE) == 0){
    return SD_DRIVER_ERR_UNEXPECTED_STATUS;
  }

  return 0;
}

// Record and report a request rejected before controller submission.
static void sd_report_request_rejection(char* operation,
    enum SdDrive drive, int start_block, int num_blocks, void* buffer,
    char* reason){
  void* args[6];
  args[0] = operation;
  args[1] = (void*)drive;
  args[2] = (void*)start_block;
  args[3] = (void*)num_blocks;
  args[4] = buffer;
  args[5] = reason;
  say("sd driver: operation=%s drive=%d block=%d count=%d buffer=0x%X rejected=%s\n",
    args);
}

// Convert an SD result code into the public diagnostic string.
static void sd_report_result(enum SdDrive drive,
    struct SdDriveContext* context, int result){
  void* args[11];
  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  args[0] = sd_operation_name(context->operation);
  args[1] = (void*)drive;
  args[2] = (void*)context->request.generation;
  args[3] = (void*)context->start_block;
  args[4] = (void*)context->num_blocks;
  args[5] = (void*)context->buffer_addr;
  args[6] = (void*)result;
  args[7] = (void*)context->last_status;
  args[8] = (void*)context->last_error;
  args[9] = (void*)context->request.quarantined;
  args[10] = (void*)context->command;
  sd_state_lock_release(&context->state_lock, state_was);
  say("sd driver: operation=%s drive=%d generation=%u block=%u count=%u buffer=0x%X result=%d status=0x%X error=%u quarantined=%d command=0x%X\n",
    args);
}

/* Validate the complete request before acquiring a drive lock or touching MMIO. */
static int sd_validate_transfer(enum SdDrive drive, int start_block,
    int num_blocks, void* buffer, char* operation){
  if (!sd_drive_is_valid(drive)){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "invalid-drive");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }
  if (start_block < 0){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "negative-start-block");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }
  if (num_blocks <= 0){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "nonpositive-block-count");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }
  if (buffer == NULL){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "null-buffer");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }

  unsigned buffer_addr = (unsigned)buffer;
  if ((buffer_addr & (SD_DMA_ALIGNMENT_BYTES - 1)) != 0){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "unaligned-buffer");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }

  unsigned block_count = (unsigned)num_blocks;
  if (block_count > SD_DMA_RAM_END_EXCLUSIVE / SD_BLOCK_SIZE_BYTES){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "byte-count-overflow");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }

  unsigned byte_count = block_count * SD_BLOCK_SIZE_BYTES;
  if (buffer_addr >= SD_DMA_RAM_END_EXCLUSIVE ||
      byte_count > SD_DMA_RAM_END_EXCLUSIVE - buffer_addr){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "ordinary-ram-range-overflow");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }

  unsigned first_block = (unsigned)start_block;
  if (block_count - 1 > UINT_MAX - first_block){
    sd_report_request_rejection(operation, drive, start_block, num_blocks,
      buffer, "block-range-overflow");
    return SD_DRIVER_ERR_INVALID_REQUEST;
  }

  return 0;
}

/*
 * Prepare a command while owning state_lock.
 *
 * The command blocking lock is already held, so there is no other legitimate
 * software writer for this drive. Holding state_lock with interrupts masked
 * makes waiter preparation, generation publication, DMA parameter stores, and
 * the CTRL store one indivisible operation with respect to the SD ISR and
 * watchdog. Sequential consistency orders all DMA parameters before START.
 */
static int sd_begin_command_locked(enum SdDrive drive,
    struct SdDriveContext* context, enum SdOperation operation,
    unsigned start_block, unsigned num_blocks, unsigned caller_buffer_addr,
    unsigned dma_buffer_addr,
    unsigned command, bool use_interrupt, unsigned* generation_out){
  unsigned status = *sd_status_reg(drive);
  unsigned error = *sd_error_reg(drive);
  context->operation = operation;
  context->start_block = start_block;
  context->num_blocks = num_blocks;
  context->buffer_addr = caller_buffer_addr;
  context->command = command;
  context->last_status = status;
  context->last_error = error;

  if (context->request.quarantined){
    return SD_DRIVER_ERR_QUARANTINED;
  }
  if (context->request.active){
    return 0 - SD_CONTROLLER_ERR_BUSY;
  }
  if ((status & SD_DMA_STATUS_BUSY) != 0){
    return 0 - SD_CONTROLLER_ERR_BUSY;
  }
  if ((status & ~SD_DMA_STATUS_KNOWN_MASK) != 0 ||
      (status & SD_DMA_STATUS_ERR) != 0 || error != 0){
    int result = sd_result_from_status(status, error);
    sd_clear_status(drive);
    return result;
  }
  if ((status & SD_DMA_STATUS_DONE) != 0){
    // A completed, inactive pre-IRQ command has no remaining owner.
    sd_clear_status(drive);
  }

  unsigned generation = sd_request_state_begin(&context->request);
  if (generation == 0){
    return context->request.quarantined ? SD_DRIVER_ERR_QUARANTINED :
      0 - SD_CONTROLLER_ERR_BUSY;
  }

  context->last_status = 0;
  context->last_error = 0;

  if (use_interrupt){
    interrupt_waiter_prepare(&context->waiter);
    unsigned now = (unsigned)__atomic_load_n((int*)&current_jiffies);
    context->deadline_jiffies = now + SD_RUNTIME_TIMEOUT_JIFFIES;
    command |= SD_DMA_CTRL_IRQ_ENABLE;
  }
  context->command = command;

  if (operation != SD_OPERATION_INIT){
    *sd_mem_reg(drive) = dma_buffer_addr;
    *sd_block_reg(drive) = start_block;
    *sd_len_reg(drive) = num_blocks;
  }
  *sd_ctrl_reg(drive) = command;

  *generation_out = generation;
  return 0;
}

/*
 * Early-boot command path. No scheduler or interrupt-driven wakeup may be used.
 * The bounded poll always terminates in success, controller error, or software
 * timeout. A timeout quarantines the controller permanently because IRQ_EN was
 * intentionally clear and no later interrupt can safely retire that command.
 */
static int sd_execute_boot_command(enum SdDrive drive,
    struct SdDriveContext* context, enum SdOperation operation,
    unsigned start_block, unsigned num_blocks, unsigned caller_buffer_addr,
    unsigned dma_buffer_addr,
    unsigned command){
  unsigned generation = 0;
  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  int result = sd_begin_command_locked(drive, context, operation,
    start_block, num_blocks, caller_buffer_addr, dma_buffer_addr,
    command & ~SD_DMA_CTRL_IRQ_ENABLE, false, &generation);
  sd_state_lock_release(&context->state_lock, state_was);

  if (result != 0){
    return result;
  }

  unsigned status = 0;
  unsigned error = 0;
  unsigned polls = 0;
  while (polls < SD_BOOT_POLL_OPERATION_LIMIT){
    status = *sd_status_reg(drive);
    error = *sd_error_reg(drive);
    if (sd_status_is_terminal(status, error)){
      break;
    }
    polls++;
  }

  state_was = sd_state_lock_acquire(&context->state_lock);
  context->last_status = status;
  context->last_error = error;

  if (!sd_status_is_terminal(status, error)){
    result = SD_DRIVER_ERR_TIMEOUT;
    sd_request_state_finish(&context->request, generation, result, true);
  } else {
    result = sd_result_from_status(status, error);
    bool still_busy = (status & SD_DMA_STATUS_BUSY) != 0;
    sd_request_state_finish_controller(&context->request, generation, result,
      still_busy);
    sd_clear_status(drive);
  }
  sd_state_lock_release(&context->state_lock, state_was);
  return result;
}

// Hold the block range and buffer passed to an SD worker thread.
struct SdBlockArgs {
  enum SdDrive drive;
  struct TCB* thread;
};

/*
 * Post-context-switch waiter publication callback.
 *
 * Preconditions: the outgoing request TCB has been completely saved and is in
 * no scheduler queue; current-core interrupts are disabled; drive generation
 * remains protected from a later command by command_lock ownership.
 *
 * If ISR/watchdog completion preceded publication, InterruptWaiter returns the
 * just-published TCB to this callback. Otherwise it leaves the TCB for the
 * future terminal publisher. Exactly one path detaches and enqueues it.
 */
static void sd_block_thread(void* arg){
  struct SdBlockArgs* args = (struct SdBlockArgs*)arg;
  struct SdDriveContext* context = &sd_contexts[args->drive];
  struct TCB* wakeup = interrupt_waiter_publish(&context->waiter,
    args->thread);

  if (wakeup != NULL){
    scheduler_wake_thread_from_interrupt(wakeup);
  }
}

// Wait for one runtime SD command to complete or time out.
static int sd_wait_runtime_command(enum SdDrive drive,
    struct SdDriveContext* context, unsigned generation){
  unsigned was = interrupts_disable();
  struct SdBlockArgs args;
  args.drive = drive;
  args.thread = get_current_tcb();
  block(was, sd_block_thread, &args, false);

  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  int result;
  if (context->request.generation != generation ||
      context->request.active){
    // This is a software state-machine failure, but return an actionable error
    // rather than converting a device request into a generic kernel panic.
    result = SD_DRIVER_ERR_UNEXPECTED_STATUS;
  } else {
    result = context->request.result;
  }
  sd_state_lock_release(&context->state_lock, state_was);
  return result;
}

// Execute one serialized SD command and publish its terminal state.
static int sd_execute_runtime_command(enum SdDrive drive,
    struct SdDriveContext* context, enum SdOperation operation,
    unsigned start_block, unsigned num_blocks, unsigned caller_buffer_addr,
    unsigned dma_buffer_addr,
    unsigned command){
  unsigned generation = 0;
  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  int result = sd_begin_command_locked(drive, context, operation,
    start_block, num_blocks, caller_buffer_addr, dma_buffer_addr, command, true,
    &generation);
  sd_state_lock_release(&context->state_lock, state_was);

  if (result != 0){
    return result;
  }
  return sd_wait_runtime_command(drive, context, generation);
}

/*
 * Reserve the drive's permanent bounce page for one command chunk.
 *
 * command_lock excludes another thread request. state_lock excludes the ISR
 * and watchdog while checking that neither an active nor quarantined
 * generation may still own the bounce page. Once this returns zero, no device
 * path can create an active generation before this caller reaches
 * sd_begin_command_locked(), so a write can copy one fixed page with
 * interrupts enabled and without holding the state lock.
 */
static int sd_prepare_bounce_chunk(enum SdDrive drive,
    struct SdDriveContext* context, enum SdOperation operation,
    unsigned start_block, unsigned caller_buffer_addr, unsigned num_blocks,
    unsigned command){
  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  bool quarantined = context->request.quarantined;
  bool active = context->request.active;
  unsigned status = *sd_status_reg(drive);
  unsigned error = *sd_error_reg(drive);
  context->operation = operation;
  context->start_block = start_block;
  context->num_blocks = num_blocks;
  context->buffer_addr = caller_buffer_addr;
  context->command = command;
  context->last_status = status;
  context->last_error = error;
  sd_state_lock_release(&context->state_lock, state_was);

  if (quarantined){
    return SD_DRIVER_ERR_QUARANTINED;
  }
  if (active || (status & SD_DMA_STATUS_BUSY) != 0){
    return 0 - SD_CONTROLLER_ERR_BUSY;
  }

  assert(num_blocks > 0 && num_blocks <= SD_DMA_BOUNCE_BLOCKS,
    "sd driver bounce: command chunk is outside the fixed bounce capacity.\n");
  if (operation == SD_OPERATION_WRITE){
    memcpy(context->bounce_buffer, (void*)caller_buffer_addr,
      num_blocks * SD_BLOCK_SIZE_BYTES);
  }
  return 0;
}

/* Caller must own context->command_lock for the complete command and wait. */
static int sd_execute_command(enum SdDrive drive,
    struct SdDriveContext* context, enum SdOperation operation,
    unsigned start_block, unsigned num_blocks, unsigned buffer_addr,
    unsigned command){
  unsigned dma_buffer_addr = buffer_addr;
  if (operation != SD_OPERATION_INIT){
    int prepare_result = sd_prepare_bounce_chunk(drive, context, operation,
      start_block, buffer_addr, num_blocks, command);
    if (prepare_result != 0){
      return prepare_result;
    }
    dma_buffer_addr = (unsigned)context->bounce_buffer;
  }

  int result;
  if (__atomic_load_n(&bootstrapping)){
    result = sd_execute_boot_command(drive, context, operation, start_block,
      num_blocks, buffer_addr, dma_buffer_addr, command);
  } else {
    result = sd_execute_runtime_command(drive, context, operation, start_block,
      num_blocks, buffer_addr, dma_buffer_addr, command);
  }

  // Expose a read only after successful terminal completion. On timeout/error
  // caller bytes remain untouched even if quarantined hardware continues to
  // write the permanent bounce page.
  if (result == 0 && operation == SD_OPERATION_READ){
    memcpy((void*)buffer_addr, context->bounce_buffer,
      num_blocks * SD_BLOCK_SIZE_BYTES);
  }
  return result;
}

/*
 * Wrapping deadline comparison copied from the sleep-queue contract. Both
 * operands are within INT_MAX ticks because SD_RUNTIME_TIMEOUT_JIFFIES is
 * bounded to that horizon. The high bit of the unsigned modular difference
 * distinguishes a not-yet-reached deadline without implementation-defined
 * unsigned-to-signed conversion.
 */
bool sd_runtime_deadline_reached(unsigned now, unsigned deadline){
  unsigned delta = now - deadline;
  return delta == 0 || (delta & (INT_MAX + 1U)) == 0;
}

/*
 * Poll one active generation from the persistent watchdog.
 *
 * The state lock makes the generation snapshot and terminal transition atomic
 * with respect to the SD ISR and command admission. If status is already
 * terminal, the watchdog returns the controller result and quarantines only
 * while BUSY says DMA still owns the fixed bounce page. It leaves DONE/ERR
 * untouched and records that the already-routed IRQ may arrive late. If status
 * has not progressed by the deadline, it publishes timeout and always applies
 * quarantine because hardware ownership is then unresolved.
 */
static void sd_watchdog_check(enum SdDrive drive, unsigned now){
  struct SdDriveContext* context = &sd_contexts[drive];
  bool publish = false;

  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  if (context->request.active){
    unsigned generation = context->request.generation;
    unsigned status = *sd_status_reg(drive);
    unsigned error = *sd_error_reg(drive);

    if (sd_status_is_terminal(status, error)){
      context->last_status = status;
      context->last_error = error;
      int result = sd_result_from_status(status, error);
      bool still_busy = (status & SD_DMA_STATUS_BUSY) != 0;
      publish = sd_request_state_finish_controller(&context->request,
        generation, result, still_busy);
      if (publish && !still_busy){
        /*
         * Do not acknowledge or clear the controller here: the routed handler
         * owns the interrupt acknowledgement. Admission may safely start a new
         * command because BUSY is clear, and the provenance bit lets that late
         * handler distinguish its nonterminal edge from new completion.
         */
        context->late_terminal_irq_pending = true;
      }
    } else if (sd_runtime_deadline_reached(now,
        context->deadline_jiffies)){
      context->last_status = status;
      context->last_error = error;
      publish = sd_request_state_finish(&context->request, generation,
        SD_DRIVER_ERR_TIMEOUT, true);
    }
  }
  sd_state_lock_release(&context->state_lock, state_was);

  if (publish){
    struct TCB* wakeup = interrupt_waiter_signal(&context->waiter);
    if (wakeup != NULL){
      scheduler_wake_thread(wakeup);
    }
  }
}

/* One bounded-storage daemon supplies the independent PIT deadline wake path. */
static void sd_watchdog(void* unused){
  (void)unused;
  while (true){
    sleep(SD_WATCHDOG_POLL_JIFFIES);
    unsigned now = (unsigned)__atomic_load_n((int*)&current_jiffies);
    sd_watchdog_check(SD_DRIVE_0, now);
    sd_watchdog_check(SD_DRIVE_1, now);
  }
}

// Initialize one controller context and its request synchronization.
static void sd_context_init(struct SdDriveContext* context){
  blocking_lock_init(&context->command_lock);
  __atomic_store_n(&context->state_lock.held, false);
  interrupt_waiter_init(&context->waiter);
  sd_request_state_init(&context->request);
  context->deadline_jiffies = 0;
  context->operation = SD_OPERATION_INIT;
  context->start_block = 0;
  context->num_blocks = 0;
  context->buffer_addr = 0;
  context->command = 0;
  context->last_status = 0;
  context->last_error = 0;
  context->late_terminal_irq_pending = false;
  context->bounce_buffer = leak(SD_DMA_BOUNCE_BYTES);
  assert(context->bounce_buffer != NULL &&
      ((unsigned)context->bounce_buffer & (SD_DMA_ALIGNMENT_BYTES - 1)) == 0 &&
      (unsigned)context->bounce_buffer < SD_DMA_RAM_END_EXCLUSIVE &&
      SD_DMA_BOUNCE_BYTES <= SD_DMA_RAM_END_EXCLUSIVE -
        (unsigned)context->bounce_buffer,
    "sd driver init: failed to allocate one aligned ordinary-RAM bounce page.\n");
}

// Initialize both SD controllers, watchdog state, and interrupt handlers.
void sd_init(void){
  sd_context_init(&sd_contexts[SD_DRIVE_0]);
  sd_context_init(&sd_contexts[SD_DRIVE_1]);

  for (int i = SD_DRIVE_0; i <= SD_DRIVE_1; i++){
    enum SdDrive drive = (enum SdDrive)i;
    struct SdDriveContext* context = &sd_contexts[drive];

    blocking_lock_acquire(&context->command_lock);
    int result = sd_execute_command(drive, context, SD_OPERATION_INIT,
      0, 0, 0, SD_DMA_CTRL_SD_INIT);
    blocking_lock_release(&context->command_lock);

    if (result != 0){
      sd_report_result(drive, context, result);
      panic("sd driver init: required controller failed; see drive/status/error diagnostic.\n");
    }
  }

  /*
   * Handlers become reachable only after boot commands have completed without
   * IRQ_EN. Runtime command state and InterruptWaiter are already initialized.
   */
  register_handler(sd0_handler_, (void*)SD_0_IVT_ENTRY);
  register_handler(sd1_handler_, (void*)SD_1_IVT_ENTRY);

  // The sole watchdog is persistent and uses no per-request allocation.
  struct Fun* watchdog_fun = leak(sizeof(struct Fun));
  watchdog_fun->func = sd_watchdog;
  watchdog_fun->arg = NULL;
  setup_thread(watchdog_fun, HIGH_PRIORITY, ANY_CORE);
}

// Stop SD workers and release controller synchronization during shutdown.
void sd_destroy(void){
  /*
   * Preconditions:
   * - All normal TCBs and all configured cores have entered shutdown.
   * - The scheduler can no longer run the persistent watchdog.
   * - No request is active, no waiter is published, and no new caller can
   *   acquire either command lock.
   */
  blocking_lock_destroy(&sd_contexts[SD_DRIVE_0].command_lock);
  blocking_lock_destroy(&sd_contexts[SD_DRIVE_1].command_lock);
  interrupt_waiter_init(&sd_contexts[SD_DRIVE_0].waiter);
  interrupt_waiter_init(&sd_contexts[SD_DRIVE_1].waiter);
  sd_request_state_init(&sd_contexts[SD_DRIVE_0].request);
  sd_request_state_init(&sd_contexts[SD_DRIVE_1].request);
}

// Validate and execute one block transfer on the selected controller.
static int sd_transfer(enum SdDrive drive, int start_block, int num_blocks,
    void* buffer, enum SdOperation operation, unsigned command){
  char* operation_name = sd_operation_name(operation);
  int result = sd_validate_transfer(drive, start_block, num_blocks, buffer,
    operation_name);
  if (result != 0){
    return result;
  }

  if (drive == SD_DRIVE_1 && start_block == 0){
    int args[2] = {(int)drive, start_block};
    say("| Warning: SD operation drive=%d block=%d accesses filesystem boot sector\n",
      args);
  }

  struct SdDriveContext* context = &sd_contexts[drive];
  blocking_lock_acquire(&context->command_lock);
  unsigned blocks_done = 0;
  unsigned total_blocks = (unsigned)num_blocks;
  while (blocks_done < total_blocks){
    unsigned chunk_blocks = total_blocks - blocks_done;
    if (chunk_blocks > SD_DMA_BOUNCE_BLOCKS){
      chunk_blocks = SD_DMA_BOUNCE_BLOCKS;
    }

    unsigned chunk_byte_offset = blocks_done * SD_BLOCK_SIZE_BYTES;
    result = sd_execute_command(drive, context, operation,
      (unsigned)start_block + blocks_done, chunk_blocks,
      (unsigned)buffer + chunk_byte_offset, command);
    if (result != 0){
      // command_lock keeps this chunk's metadata stable through diagnostic.
      sd_report_result(drive, context, result);
      break;
    }
    blocks_done += chunk_blocks;
  }
  blocking_lock_release(&context->command_lock);
  return result;
}

// Read complete SD sectors into a kernel buffer.
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks,
    void* dest){
  return sd_transfer(drive, start_block, num_blocks, dest,
    SD_OPERATION_READ, SD_DMA_CTRL_START);
}

// Write complete SD sectors from a kernel buffer.
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks,
    void* src){
  return sd_transfer(drive, start_block, num_blocks, src,
    SD_OPERATION_WRITE, SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD);
}

// Consume one controller interrupt and advance its request state machine.
void sd_handler(enum SdDrive drive){
  if (!sd_drive_is_valid(drive)){
    int arg = (int)drive;
    say("sd driver: operation=interrupt invalid drive=%d\n", &arg);
    return;
  }

  // Acknowledge only this controller before examining its sticky MMIO status.
  if (drive == SD_DRIVE_0){
    mark_sd0_handled();
  } else {
    mark_sd1_handled();
  }

  struct SdDriveContext* context = &sd_contexts[drive];
  bool publish = false;
  bool unexpected_interrupt = false;
  unsigned status;
  unsigned error;
  unsigned generation;

  unsigned state_was = sd_state_lock_acquire(&context->state_lock);
  status = *sd_status_reg(drive);
  error = *sd_error_reg(drive);
  generation = context->request.generation;
  bool expected_late_irq = context->late_terminal_irq_pending;
  context->late_terminal_irq_pending = false;

  if (!context->request.active && context->request.quarantined &&
      sd_status_is_terminal(status, error)){
    /*
     * This IRQ belongs to the timed-out generation. Clear sticky controller
     * state while holding the same lock checked by admission. Quarantine may
     * be released only after BUSY clears; terminal ERR/DONE with BUSY still set
     * does not release the non-atomic DMA engine's ownership of the bounce
     * page. Only after that ownership boundary can a later generation start,
     * so this IRQ cannot satisfy it.
     */
    context->last_status = status;
    context->last_error = error;
    sd_clear_status(drive);
    if ((status & SD_DMA_STATUS_BUSY) == 0){
      sd_request_state_acknowledge_quarantine(&context->request,
        context->request.generation);
    }
  } else if (context->request.active &&
      sd_status_is_terminal(status, error)){
    int result = sd_result_from_status(status, error);
    bool still_busy = (status & SD_DMA_STATUS_BUSY) != 0;
    context->last_status = status;
    context->last_error = error;
    sd_clear_status(drive);
    publish = sd_request_state_finish_controller(&context->request,
      generation, result, still_busy);
  } else if (expected_late_irq){
    /*
     * The watchdog already published this terminal command. If no newer
     * command is active, discard its sticky terminal status. If a newer
     * command is active and nonterminal, this is only the delayed old edge and
     * its status must remain untouched. An active terminal command was handled
     * by the preceding branch and is safely allowed to consume a coalesced IRQ.
     */
    if (sd_status_is_terminal(status, error)){
      context->last_status = status;
      context->last_error = error;
      sd_clear_status(drive);
    }
  } else {
    /*
     * A nonterminal or ownerless IRQ is acknowledged but never wakes a TCB.
     * If a command is active, its independent watchdog remains responsible for
     * eventual completion/timeout. Clear only terminal sticky state.
     */
    unexpected_interrupt = true;
    if (sd_status_is_terminal(status, error)){
      sd_clear_status(drive);
    }
  }
  sd_state_lock_release(&context->state_lock, state_was);

  if (publish){
    struct TCB* wakeup = interrupt_waiter_signal(&context->waiter);
    if (wakeup != NULL){
      scheduler_wake_thread_from_interrupt(wakeup);
    }
  } else if (unexpected_interrupt){
    void* args[4];
    args[0] = (void*)drive;
    args[1] = (void*)generation;
    args[2] = (void*)status;
    args[3] = (void*)error;
    say("sd driver: operation=interrupt drive=%d generation=%u status=0x%X error=%u had no terminal owner\n",
      args);
  }
}
