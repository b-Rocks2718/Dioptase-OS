#include "../kernel/audio.h"
#include "../kernel/config.h"
#include "../kernel/debug.h"
#include "../kernel/pit.h"
#include "../kernel/print.h"
#include "../kernel/threads.h"

/*
 * Summary:
 *   Generate a very short square-wave cue, queue it entirely in the audio MMIO
 *   ring buffer, and verify that the emulator consumes it.
 *
 * Why this exists:
 *   This smoke test validates the emulator audio path without depending on WAV
 *   parsing or filesystem reads. The cue is intentionally brief so the test
 *   can complete within the existing emulator timeout budget, and it proves
 *   basic device progress by checking that AUDIO_READ_IDX advances after
 *   playback starts.
 *
 * How:
 *   The test fills the documented ring buffer with synthesized signed 16-bit
 *   mono PCM samples, enables playback, then waits for that fixed queued
 *   region to drain before disabling the device.
 */

#define TONE_AMPLITUDE 16000
#define TONE_SEGMENT_COUNT 5
#define AUDIO_PROGRESS_WAIT_JIFFIES 10
#define AUDIO_PROGRESS_TIMEOUT_JIFFIES 200
#define AUDIO_DRAIN_TIMEOUT_JIFFIES 12000

struct ToneGenerator {
  unsigned segment_idx;
  unsigned segment_progress;
  unsigned toggle_countdown;
  int sample_value;
};

static unsigned tone_segment_samples(unsigned segment_idx){
  switch (segment_idx) {
    case 0: return 100; /* short leading silence */
    case 1: return 350; /* ~14 ms at ~440 Hz */
    case 2: return 100; /* short pause */
    case 3: return 350; /* ~14 ms at ~658 Hz */
    case 4: return 100; /* short trailing silence */
    default: return 0;
  }
}

static unsigned tone_half_period_samples(unsigned segment_idx){
  switch (segment_idx) {
    case 1: return 28;
    case 3: return 19;
    default: return 0;
  }
}

static bool tone_has_more(struct ToneGenerator* tone){
  return tone->segment_idx < TONE_SEGMENT_COUNT;
}

static int tone_next_sample(struct ToneGenerator* tone){
  while (tone->segment_idx < TONE_SEGMENT_COUNT){
    unsigned segment_samples = tone_segment_samples(tone->segment_idx);
    unsigned half_period_samples = tone_half_period_samples(tone->segment_idx);

    if (tone->segment_progress >= segment_samples){
      tone->segment_idx++;
      tone->segment_progress = 0;
      tone->toggle_countdown = 0;
      tone->sample_value = TONE_AMPLITUDE;
      continue;
    }

    tone->segment_progress++;

    if (half_period_samples == 0){
      return 0;
    }

    if (tone->toggle_countdown == 0){
      tone->toggle_countdown = half_period_samples;
    }

    {
      int sample = tone->sample_value;

      tone->toggle_countdown--;
      if (tone->toggle_countdown == 0){
        tone->sample_value = -tone->sample_value;
      }

      return sample;
    }
  }

  return 0;
}

/*
 * Snapshot the consumer index once, then publish as many generated samples as
 * fit before updating the producer index. This smoke test intentionally keeps
 * the whole cue inside one ring-buffer fill so the completion condition is just
 * "consumer catches producer" with no dependence on LOW_WATER refill timing.
 */
static void audio_fill_generated_samples(struct ToneGenerator* tone){
  unsigned read_idx = audio_output_read_idx();
  unsigned write_idx = audio_output_write_idx();

  while (tone_has_more(tone) &&
         audio_output_free_bytes(write_idx, read_idx) >= AUDIO_SAMPLE_BYTES){
    audio_output_write_sample_s16le(tone_next_sample(tone), write_idx);
    write_idx = audio_output_advance_idx(write_idx, AUDIO_SAMPLE_BYTES);
  }

  audio_output_set_write_idx(write_idx);
}

int kernel_main(void){
  struct ToneGenerator tone;
  unsigned initial_read_idx;
  unsigned progressed_read_idx;
  unsigned progress_wait = AUDIO_PROGRESS_TIMEOUT_JIFFIES;
  unsigned drain_wait = AUDIO_DRAIN_TIMEOUT_JIFFIES;
  int progress_args[2];
  int drain_args[3];

  say("***audio smoke start\n", NULL);

  if (!CONFIG.use_audio){
    say("| Host audio sink disabled; validating device consumption only\n", NULL);
  }

  tone.segment_idx = 0;
  tone.segment_progress = 0;
  tone.toggle_countdown = 0;
  tone.sample_value = TONE_AMPLITUDE;

  audio_output_reset(0);
  audio_fill_generated_samples(&tone);
  if (tone_has_more(&tone)){
    panic("audio smoke: fixed cue did not fit in the audio ring buffer\n");
  }
  audio_output_enable();

  initial_read_idx = audio_output_read_idx();
  progressed_read_idx = initial_read_idx;
  while (progress_wait != 0 && progressed_read_idx == initial_read_idx){
    sleep(AUDIO_PROGRESS_WAIT_JIFFIES);
    progressed_read_idx = audio_output_read_idx();
    if (progress_wait >= AUDIO_PROGRESS_WAIT_JIFFIES){
      progress_wait -= AUDIO_PROGRESS_WAIT_JIFFIES;
    } else {
      progress_wait = 0;
    }
  }

  if (progressed_read_idx == initial_read_idx){
    progress_args[0] = initial_read_idx;
    progress_args[1] = progressed_read_idx;
    say("***audio smoke FAIL read_idx start=0x%X current=0x%X\n", progress_args);
    panic("audio smoke: device did not consume queued samples\n");
  }

  say("***audio smoke device progress ok\n", NULL);

  while (!audio_output_empty()){
    if (drain_wait == 0){
      drain_args[0] = audio_output_read_idx();
      drain_args[1] = audio_output_write_idx();
      drain_args[2] = audio_output_status();
      say("***audio smoke FAIL drain read_idx=0x%X write_idx=0x%X status=0x%X\n",
          drain_args);
      panic("audio smoke: timed out waiting for the ring buffer to drain\n");
    }
    sleep(1);
    drain_wait--;
  }

  audio_output_disable();
  say("***audio smoke complete\n", NULL);

  return 0;
}
