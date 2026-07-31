/*
 * user_synth_audio guest:
 * - validates that get_synth_audio() maps the synth MMIO page into user space
 * - creates a tiny DSYN v1 command stream as a regular file
 * - scans and plays that DSYN file through the CRT synth_audio helpers
 * - verifies playback wrote the expected register values into the synth page
 */

#include "../../../root/crt/fcntl.h"
#include "../../../root/crt/synth_audio.h"
#include "../../../root/crt/sys.h"
#include "../../../root/crt/unistd.h"
#include "../../user_test.h"

#define TEST_DSYN_EVENTS 8u
#define TEST_DSYN_BYTES 128u
#define TEST_MASTER_VOLUME 31u
#define TEST_SQUARE_VOLUME 77u
#define TEST_SQUARE_TIMER 10u
#define TEST_SQUARE_LENGTH 20u
#define TEST_SQUARE_CTRL 35u
#define TEST_TOTAL_SAMPLES 5u

static void write_u32_le(char* bytes, unsigned value){
  bytes[0] = value & 0xFF;
  bytes[1] = (value >> 8) & 0xFF;
  bytes[2] = (value >> 16) & 0xFF;
  bytes[3] = (value >> 24) & 0xFF;
}

static void fill_dsyn_event(char* bytes, unsigned event_index,
    unsigned delta_samples, unsigned reg_offset, unsigned value){
  unsigned offset = DSYN_HEADER_BYTES + event_index * DSYN_EVENT_BYTES;

  write_u32_le(bytes + offset, delta_samples);
  write_u32_le(bytes + offset + 4, reg_offset);
  write_u32_le(bytes + offset + 8, value);
}

static void fill_test_dsyn(char* bytes){
  bytes[0] = 'D';
  bytes[1] = 'S';
  bytes[2] = 'Y';
  bytes[3] = 'N';
  write_u32_le(bytes + 4, DSYN_VERSION);
  write_u32_le(bytes + 8, DSYN_HEADER_BYTES);
  write_u32_le(bytes + 12, SYNTH_AUDIO_SAMPLE_RATE_HZ);
  write_u32_le(bytes + 16, TEST_DSYN_EVENTS);
  write_u32_le(bytes + 20, DSYN_NO_LOOP);
  write_u32_le(bytes + 24, DSYN_FLAGS_NONE);
  write_u32_le(bytes + 28, 0);

  fill_dsyn_event(bytes, 0, 0, SYNTH_AUDIO_CTRL_OFFSET,
      SYNTH_AUDIO_CTRL_RESET_STATE | SYNTH_AUDIO_CTRL_ENABLE);
  fill_dsyn_event(bytes, 1, 0, SYNTH_AUDIO_MASTER_VOLUME_OFFSET,
      TEST_MASTER_VOLUME);
  fill_dsyn_event(bytes, 2, 0,
      SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_TIMER_OFFSET,
      TEST_SQUARE_TIMER);
  fill_dsyn_event(bytes, 3, 0,
      SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_VOLUME_OFFSET,
      TEST_SQUARE_VOLUME);
  fill_dsyn_event(bytes, 4, 0,
      SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_LENGTH_OFFSET,
      TEST_SQUARE_LENGTH);
  fill_dsyn_event(bytes, 5, 0,
      SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_CTRL_OFFSET,
      TEST_SQUARE_CTRL);
  fill_dsyn_event(bytes, 6, 0,
      SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_TRIGGER_OFFSET,
      SYNTH_AUDIO_CH_TRIGGER_START);
  fill_dsyn_event(bytes, 7, 5, SYNTH_AUDIO_CTRL_OFFSET, 0);
}

int main(void){
  char dsyn[TEST_DSYN_BYTES];
  struct DsynStats stats;
  unsigned* regs = get_synth_audio();
  unsigned square_volume_index =
    (SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_VOLUME_OFFSET) /
    SYNTH_AUDIO_REG_BYTES;
  int fd;

  user_test_expect_eq("map synth audio registers",
    (int)regs != 0 && (int)regs != -1, 1);
  user_test_expect_eq("synth command-ring version",
    regs[SYNTH_AUDIO_VERSION_OFFSET / SYNTH_AUDIO_REG_BYTES],
    SYNTH_AUDIO_COMMAND_RING_VERSION);
  user_test_expect_eq("synth sample rate",
    regs[SYNTH_AUDIO_SAMPLE_RATE_HZ_OFFSET / SYNTH_AUDIO_REG_BYTES],
    SYNTH_AUDIO_SAMPLE_RATE_HZ);

  fill_test_dsyn(dsyn);

  fd = open("tiny.dsyn");
  user_test_expect_eq("create DSYN fixture", fd >= 0, 1);
  user_test_expect_eq("write complete DSYN fixture",
    write(fd, dsyn, TEST_DSYN_BYTES), TEST_DSYN_BYTES);
  user_test_expect_eq("seek(fd, 0, SEEK_SET)", seek(fd, 0, SEEK_SET), 0);
  user_test_expect_eq("synth_audio_play_dsyn_fd(fd)", synth_audio_play_dsyn_fd(fd), 0);
  user_test_expect_eq("close(fd)", close(fd), 0);

  user_test_expect_eq("playback master volume register",
    regs[SYNTH_AUDIO_MASTER_VOLUME_OFFSET / SYNTH_AUDIO_REG_BYTES],
    TEST_MASTER_VOLUME);
  user_test_expect_eq("playback square volume register",
    regs[square_volume_index], TEST_SQUARE_VOLUME);

  fd = open("tiny.dsyn");
  user_test_expect_eq("reopen DSYN fixture for scan", fd >= 0, 1);
  user_test_expect_eq("synth_audio_scan_dsyn_fd(fd, &stats)", synth_audio_scan_dsyn_fd(fd, &stats), 0);
  user_test_expect_eq("scanned DSYN event count", stats.header.event_count,
    TEST_DSYN_EVENTS);
  user_test_expect_eq("scanned DSYN maximum register offset",
    stats.max_reg_offset,
    SYNTH_AUDIO_SQUARE0_OFFSET + SYNTH_AUDIO_CH_TRIGGER_OFFSET);
  user_test_expect_eq("scanned DSYN total samples", stats.total_samples,
    TEST_TOTAL_SAMPLES);
  user_test_expect_eq("close(fd)", close(fd), 0);

  return 0;
}
