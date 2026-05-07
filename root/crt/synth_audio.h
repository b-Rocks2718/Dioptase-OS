#ifndef SYNTH_AUDIO_H
#define SYNTH_AUDIO_H

/*
 * User-space helpers for the register-driven synth audio MMIO device.
 *
 * Hardware contract:
 * - docs/mem_map.md defines the synth page as 0x7FBC000 - 0x7FBCFFF.
 * - get_synth_audio() maps that page into the caller's user address space.
 * - All register offsets below are byte offsets within that mapped page.
 *
 * Timing contract:
 * - DSYN event deltas are counted in synth output samples at 25 kHz.
 * - Synth device version 4 exposes a timestamped command ring. Playback
 *   requires that ring so batches of register writes can be queued ahead of
 *   batched host audio rendering with one producer-index publication.
 */

#define SYNTH_AUDIO_PAGE_BYTES 0x1000

#define SYNTH_AUDIO_CTRL_OFFSET 0x000
#define SYNTH_AUDIO_STATUS_OFFSET 0x004
#define SYNTH_AUDIO_MASTER_VOLUME_OFFSET 0x008
#define SYNTH_AUDIO_VERSION_OFFSET 0x00C
#define SYNTH_AUDIO_CLOCK_HZ_OFFSET 0x010
#define SYNTH_AUDIO_SAMPLE_RATE_HZ_OFFSET 0x014
#define SYNTH_AUDIO_SAMPLE_COUNTER_OFFSET 0x018

#define SYNTH_AUDIO_CMD_STATUS_OFFSET 0x120
#define SYNTH_AUDIO_CMD_WRITE_IDX_OFFSET 0x124
#define SYNTH_AUDIO_CMD_READ_IDX_OFFSET 0x128
#define SYNTH_AUDIO_CMD_CTRL_OFFSET 0x12C
#define SYNTH_AUDIO_CMD_RING_BASE_OFFSET 0x130
#define SYNTH_AUDIO_CMD_RING_BYTES_OFFSET 0x134
#define SYNTH_AUDIO_CMD_RECORD_BYTES_OFFSET 0x138
#define SYNTH_AUDIO_CMD_RING_OFFSET 0x200
#define SYNTH_AUDIO_CMD_RING_BYTES 0xE00
#define SYNTH_AUDIO_CMD_RECORD_BYTES 16u
/* 0xE00 bytes / 16 bytes per record = 224 records; one slot stays unused. */
#define SYNTH_AUDIO_CMD_RING_USABLE_COMMANDS 223u
#define SYNTH_AUDIO_CMD_TARGET_SAMPLE_OFFSET 0x00
#define SYNTH_AUDIO_CMD_REG_OFFSET_OFFSET 0x04
#define SYNTH_AUDIO_CMD_VALUE_OFFSET 0x08
#define SYNTH_AUDIO_CMD_FLAGS_OFFSET 0x0C

#define SYNTH_AUDIO_CTRL_ENABLE 0x1
#define SYNTH_AUDIO_CTRL_RESET_STATE 0x2

#define SYNTH_AUDIO_CMD_STATUS_FULL 0x1
#define SYNTH_AUDIO_CMD_STATUS_EMPTY 0x2
#define SYNTH_AUDIO_CMD_STATUS_LATE 0x4
#define SYNTH_AUDIO_CMD_STATUS_OVERFLOW 0x8
#define SYNTH_AUDIO_CMD_STATUS_BAD_OFFSET 0x10
#define SYNTH_AUDIO_CMD_STATUS_BAD_FLAGS 0x20
#define SYNTH_AUDIO_CMD_STATUS_COUNT_SHIFT 8
#define SYNTH_AUDIO_CMD_STATUS_SPACE_SHIFT 20
#define SYNTH_AUDIO_CMD_CTRL_RESET 0x1
#define SYNTH_AUDIO_CMD_CTRL_CLEAR_FLAGS 0x2

#define SYNTH_AUDIO_CHANNEL_STRIDE 0x20

#define SYNTH_AUDIO_SQUARE0_OFFSET 0x020
#define SYNTH_AUDIO_SQUARE1_OFFSET 0x040
#define SYNTH_AUDIO_SQUARE2_OFFSET 0x060
#define SYNTH_AUDIO_SQUARE3_OFFSET 0x080
#define SYNTH_AUDIO_TRIANGLE0_OFFSET 0x0A0
#define SYNTH_AUDIO_TRIANGLE1_OFFSET 0x0C0
#define SYNTH_AUDIO_NOISE0_OFFSET 0x0E0
#define SYNTH_AUDIO_NOISE1_OFFSET 0x100

#define SYNTH_AUDIO_CH_CTRL_OFFSET 0x00
#define SYNTH_AUDIO_CH_TIMER_OFFSET 0x04
#define SYNTH_AUDIO_CH_VOLUME_OFFSET 0x08
#define SYNTH_AUDIO_CH_LENGTH_OFFSET 0x0C
#define SYNTH_AUDIO_CH_PHASE_OFFSET 0x10
#define SYNTH_AUDIO_CH_TRIGGER_OFFSET 0x14

#define SYNTH_AUDIO_CH_ENABLE 0x1
#define SYNTH_AUDIO_CH_LENGTH_ENABLE 0x2
#define SYNTH_AUDIO_CH_TRIGGER_START 0x1

#define SYNTH_AUDIO_SQUARE_DUTY_SHIFT 4
#define SYNTH_AUDIO_NOISE_SHORT_MODE 0x10

#define SYNTH_AUDIO_CLOCK_HZ 100000000u
#define SYNTH_AUDIO_SAMPLE_RATE_HZ 25000u
#define SYNTH_AUDIO_COMMAND_RING_VERSION 4u
#define SYNTH_AUDIO_DSYN_RING_LEAD_SAMPLES 8192u
#define SYNTH_AUDIO_DSYN_RING_MIN_LEAD_SAMPLES 512u
#define SYNTH_AUDIO_DSYN_RING_BATCH_COMMANDS 32u

#define SYNTH_AUDIO_REG_BYTES 4u

#define DSYN_VERSION 1u
#define DSYN_HEADER_BYTES 32u
#define DSYN_EVENT_BYTES 12u
#define DSYN_NO_LOOP 0xFFFFFFFFu
#define DSYN_FLAGS_NONE 0u

#define DSYN_OK 0
#define DSYN_ERR_READ -1
#define DSYN_ERR_TRUNCATED -2
#define DSYN_ERR_BAD_MAGIC -3
#define DSYN_ERR_BAD_VERSION -4
#define DSYN_ERR_BAD_HEADER_SIZE -5
#define DSYN_ERR_BAD_SAMPLE_RATE -6
#define DSYN_ERR_UNSUPPORTED_LOOP -7
#define DSYN_ERR_UNSUPPORTED_FLAGS -8
#define DSYN_ERR_BAD_EVENT_OFFSET -9
#define DSYN_ERR_SYNTH_MAP -10
#define DSYN_ERR_BAD_ARG -11
#define DSYN_ERR_SYNTH_RING_UNSUPPORTED -12
#define DSYN_ERR_SYNTH_RING_LATE -13
#define DSYN_ERR_SYNTH_RING_OVERFLOW -14
#define DSYN_ERR_SYNTH_RING_BAD_OFFSET -15
#define DSYN_ERR_SYNTH_RING_BAD_FLAGS -16

struct DsynHeader {
  unsigned version;
  unsigned header_size;
  unsigned sample_rate_hz;
  unsigned event_count;
  unsigned loop_start_event;
  unsigned flags;
  unsigned reserved;
};

struct DsynEvent {
  unsigned delta_samples;
  unsigned reg_offset;
  unsigned value;
};

struct DsynStats {
  struct DsynHeader header;
  unsigned total_samples;
  unsigned max_delta_samples;
  unsigned max_reg_offset;
};

int synth_audio_read_dsyn_header_fd(int fd, struct DsynHeader* header);
int synth_audio_read_dsyn_event_fd(int fd, struct DsynEvent* event);
int synth_audio_validate_dsyn_header(struct DsynHeader* header);
int synth_audio_validate_dsyn_event(struct DsynEvent* event);
int synth_audio_scan_dsyn_fd(int fd, struct DsynStats* stats);
int synth_audio_play_dsyn_fd(int fd);
void synth_audio_stop(unsigned* regs);
char* synth_audio_error_string(int err);

#endif // SYNTH_AUDIO_H
