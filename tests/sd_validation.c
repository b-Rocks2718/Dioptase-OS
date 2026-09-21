/*
 * SD request-admission and generation-state regression test.
 *
 * Validates:
 * - one generation accepts exactly one terminal result
 * - timeout quarantine rejects a new generation until matching acknowledgement
 * - terminal watchdog completion without BUSY permits the next generation
 * - terminal controller state with BUSY retains bounce-page quarantine
 * - a late result for an old generation cannot finish a newer request
 * - malformed drive/block/count/buffer/range inputs fail before DMA MMIO
 * - wrapping runtime deadlines use unsigned half-range arithmetic
 * - rejected requests leave the controller usable for a later valid read
 *
 * The state-machine checks use a local SdRequestState, so timeout and stale-IRQ
 * transitions are deterministic and do not require hanging the emulator's real
 * SD engine.
 */

#include "../kernel/constants.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/sd_driver.h"

#define TEST_BLOCK_BYTES 512
#define TEST_PHYS_ADDR_END_EXCLUSIVE 0x08000000
#define TEST_MMIO_START 0x07FB8000
#define TEST_CHUNKED_BLOCKS 9
#define TEST_CHUNKED_BYTES 4608

static void test_generation_arbitration(void){ /* Verify one terminal publisher wins per generation and stale generations cannot interfere. */
  struct SdRequestState state;
  sd_request_state_init(&state);

  unsigned first = sd_request_state_begin(&state);
  assert(first != 0 && state.active,
    "sd validation: first generation did not begin.\n");
  assert(sd_request_state_finish(&state, first, SD_DRIVER_ERR_TIMEOUT, true),
    "sd validation: timeout did not publish first terminal result.\n");
  assert(!sd_request_state_finish(&state, first, 0, false),
    "sd validation: one generation accepted two terminal publishers.\n");
  assert(sd_request_state_begin(&state) == 0,
    "sd validation: quarantined state admitted a new generation.\n");
  assert(!sd_request_state_acknowledge_quarantine(&state, first + 1),
    "sd validation: stale acknowledgement cleared quarantine.\n");
  assert(state.quarantined,
    "sd validation: rejected acknowledgement changed quarantine.\n");
  assert(sd_request_state_acknowledge_quarantine(&state, first),
    "sd validation: matching late acknowledgement did not clear quarantine.\n");

  unsigned second = sd_request_state_begin(&state);
  assert(second != 0 && second != first,
    "sd validation: second generation was not unique.\n");
  assert(!sd_request_state_finish(&state, first, 0, false),
    "sd validation: stale generation finished the active request.\n");
  assert(state.active && state.generation == second,
    "sd validation: stale finish changed the active generation.\n");
  assert(sd_request_state_finish(&state, second, 0, false),
    "sd validation: current generation did not accept completion.\n");
  assert(!state.active && !state.quarantined && state.result == 0,
    "sd validation: successful terminal state is inconsistent.\n");
}

static void test_terminal_controller_policy(void){ /* Verify controller completion retains quarantine only while hardware remains busy. */
  struct SdRequestState state;
  sd_request_state_init(&state);

  unsigned completed = sd_request_state_begin(&state);
  assert(completed != 0,
    "sd validation: terminal-policy generation did not begin.\n");
  assert(sd_request_state_finish_controller(&state, completed, 0, false),
    "sd validation: clean terminal watchdog result was not published.\n");
  assert(!state.active && !state.quarantined,
    "sd validation: clean terminal watchdog result retained quarantine.\n");

  unsigned next = sd_request_state_begin(&state);
  assert(next != 0 && next != completed,
    "sd validation: clean terminal watchdog result blocked the next request.\n");
  assert(sd_request_state_finish_controller(&state, next,
      SD_DRIVER_ERR_UNEXPECTED_STATUS, true),
    "sd validation: BUSY terminal controller result was not published.\n");
  assert(state.quarantined && sd_request_state_begin(&state) == 0,
    "sd validation: BUSY terminal controller result released DMA ownership.\n");
  assert(sd_request_state_acknowledge_quarantine(&state, next),
    "sd validation: BUSY terminal quarantine could not be acknowledged.\n");
}

static void test_wrapping_deadline(void){ /* Verify deadline comparisons remain correct across the runtime counter wrap. */
  unsigned deadline = 2;
  assert(!sd_runtime_deadline_reached(UINT_MAX - 2, deadline),
    "sd validation: pre-wrap time reached a post-wrap deadline early.\n");
  assert(!sd_runtime_deadline_reached(1, deadline),
    "sd validation: tick before deadline was reported reached.\n");
  assert(sd_runtime_deadline_reached(deadline, deadline),
    "sd validation: exact deadline was not reported reached.\n");
  assert(sd_runtime_deadline_reached(3, deadline),
    "sd validation: tick after deadline was not reported reached.\n");
}

static void expect_invalid(int result, char* message){ /* Require request validation to reject an input before touching the controller. */
  assert(result == SD_DRIVER_ERR_INVALID_REQUEST, message);
}

static void test_request_validation(char* buffer){ /* Exercise invalid drive, range, count, and DMA-buffer requests. */
  expect_invalid(sd_read_blocks((enum SdDrive)2, 0, 1, buffer),
    "sd validation: invalid drive reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, -1, 1, buffer),
    "sd validation: negative block reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, 0, 0, buffer),
    "sd validation: zero block count reached the controller.\n");
  expect_invalid(sd_write_blocks(SD_DRIVE_0, 0, 1, NULL),
    "sd validation: NULL DMA buffer reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, 0, 1, buffer + 1),
    "sd validation: unaligned DMA buffer reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, 0, INT_MAX, buffer),
    "sd validation: overflowing DMA byte count reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, 0, 1,
      (void*)(TEST_PHYS_ADDR_END_EXCLUSIVE - 4)),
    "sd validation: out-of-range DMA span reached the controller.\n");
  expect_invalid(sd_read_blocks(SD_DRIVE_0, 0, 1, (void*)TEST_MMIO_START),
    "sd validation: MMIO caller buffer reached the staged controller path.\n");
}

int kernel_main(void){ /* Exercise SD request validation and timeout state transitions. */
  say("***sd validation test start\n", NULL);

  test_generation_arbitration();
  test_terminal_controller_policy();
  test_wrapping_deadline();

  char* buffer = malloc(TEST_BLOCK_BYTES);
  assert(buffer != NULL,
    "sd validation: failed to allocate aligned block buffer.\n");
  assert(((unsigned)buffer & 3) == 0,
    "sd validation: heap returned an unaligned DMA buffer.\n");
  test_request_validation(buffer);

  int result = sd_read_blocks(SD_DRIVE_0, 0, 1, buffer);
  assert(result == 0,
    "sd validation: valid read failed after rejected requests.\n");
  free(buffer);

  char* chunked_buffer = malloc(TEST_CHUNKED_BYTES);
  assert(chunked_buffer != NULL,
    "sd validation: failed to allocate chunked-read buffer.\n");
  result = sd_read_blocks(SD_DRIVE_0, 0, TEST_CHUNKED_BLOCKS,
    chunked_buffer);
  assert(result == 0,
    "sd validation: request larger than one bounce page did not chunk.\n");
  free(chunked_buffer);

  say("***sd validation state/request checks ok\n", NULL);
  say("***sd validation test complete\n", NULL);
  return 0;
}
