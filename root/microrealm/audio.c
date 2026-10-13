#include "realm.h"
#include "../crt/sys.h"

/* 20 ms blocks at the documented 25 kHz sample rate; refill to 120 ms.
 * Counts passed to the CRT are bytes, while synthesis tracks 16-bit samples.
 */
#define AUDIO_BUF_SAMPLES 500
#define AUDIO_TARGET_BYTES 6000
static short audio_mix_buf[AUDIO_BUF_SAMPLES];

static int audio_device_claimed = 0;
/* The single game thread retains each block until every sample is accepted. */
static int pending_samples = 0;
static int pending_offset = 0;

/* Synthesizer voice state. */
static int sfx_type = 0;
static int sfx_phase = 0;
static int sfx_step = 0;
static int sfx_amp = 0;
static int sfx_decay = 0;
static int sfx_noise = 0xACE1;
static int sfx_fanfare_timer = 0;

/* Musical sequencer state. */
static int music_note_idx = 0;
static int music_sample_timer = 0;
static int music_phase_accum = 0;

#define MELODY_LENGTH 12

/* Precomputed Dorian pitch steps per sample at 25 kHz. */
static int melody_pitches[MELODY_LENGTH] = {
  768, 914, 1027, 1153, 1371, 1153, 1027, 914, 768, 865, 914, 768
};

/* Initialize the hardware audio controller ring. */
void audio_init(void) {
  if (audio_open() == 0) {
    audio_device_claimed = 1;
    pending_samples = 0;
    pending_offset = 0;
  }
}

/* Gracefully release hardware audio device. */
void audio_shutdown(void) {
  if (audio_device_claimed) {
    audio_close();
    audio_device_claimed = 0;
    pending_samples = 0;
  }
}

/* Queue a one-shot audio sound effect to play immediately. */
void audio_play_sfx(int sfx_id) {
  sfx_phase = 0;

  switch (sfx_id) {
    case SFX_WOOD_CHOP:
      sfx_type = 1; /* Noise */
      sfx_amp = 3500;
      sfx_decay = 3;
      break;

    case SFX_STONE_MINE:
      sfx_type = 2; /* 1200 Hz Tone */
      sfx_step = 3145;
      sfx_amp = 3000;
      sfx_decay = 2;
      break;

    case SFX_ARROW_SHOOT:
      sfx_type = 1; /* Whoosh noise */
      sfx_amp = 2000;
      sfx_decay = 2;
      break;

    case SFX_SWORD_HIT:
      sfx_type = 2; /* 880 Hz Metal ring */
      sfx_step = 2306;
      sfx_amp = 4000;
      sfx_decay = 3;
      break;

    case SFX_BUILD_PLACE:
      sfx_type = 2; /* 140 Hz Thud */
      sfx_step = 367;
      sfx_amp = 5000;
      sfx_decay = 2;
      break;

    case SFX_BELL_DUSK:
      sfx_type = 2; /* 330 Hz Cathedral bell */
      sfx_step = 865;
      sfx_amp = 6000;
      sfx_decay = 1;
      break;

    case SFX_FANFARE_DAWN:
      sfx_type = 3; /* Royal fanfare arpeggio */
      sfx_step = 687;
      sfx_amp = 4500;
      sfx_decay = 1;
      sfx_fanfare_timer = 0;
      break;

    case SFX_GOLD_CHIME:
      sfx_type = 2; /* 1760 Hz High chime */
      sfx_step = 4613;
      sfx_amp = 4000;
      sfx_decay = 3;
      break;

    case SFX_DEFEAT:
      sfx_type = 2; /* 110 Hz Low toll */
      sfx_step = 288;
      sfx_amp = 5000;
      sfx_decay = 1;
      break;

    default:
      sfx_amp = 0;
      break;
  }
}

/* Generate one owned PCM block using phase increments and linear envelopes.
 * This is single-threaded user-mode state; no audio MMIO is accessed directly.
 */
static void synthesize_block(struct GameWorld* w) {
  int night_mode = (w->phase == PHASE_NIGHT);

  /* Synthesize next audio block with fast shifts and additions */
  for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
    int sample = 0;

    /* Background melody sequencer */
    music_sample_timer++;
    if (music_sample_timer >= 3000) {
      music_sample_timer = 0;
      music_note_idx++;
      if (music_note_idx >= MELODY_LENGTH) {
        music_note_idx = 0;
      }
    }

    int step = melody_pitches[music_note_idx];
    if (night_mode) {
      /* Night battle tension: pitch up by 5/4 = step + (step >> 2) */
      step += (step >> 2);
    }
    music_phase_accum = (music_phase_accum + step) & 0xFFFF;

    /* Triangle wave: zero multiply */
    int tri = (music_phase_accum < 32768) ? (music_phase_accum - 16384) : (49152 - music_phase_accum);
    sample += (tri >> 4);

    /* Sound effect voice */
    if (sfx_amp > 0) {
      int sfx_val = 0;

      if (sfx_type == 1) {
        /* Galois LFSR noise: zero multiply */
        sfx_noise = (sfx_noise >> 1) ^ (-(sfx_noise & 1) & 0xB400);
        sfx_val = (sfx_noise & 1) ? sfx_amp : -sfx_amp;
      } else if (sfx_type == 2) {
        /* Square tone: zero multiply */
        sfx_phase = (sfx_phase + sfx_step) & 0xFFFF;
        sfx_val = (sfx_phase < 32768) ? sfx_amp : -sfx_amp;
      } else if (sfx_type == 3) {
        /* Arpeggio fanfare: zero multiply */
        sfx_fanfare_timer++;
        if (sfx_fanfare_timer == 1200) sfx_step = 865;
        else if (sfx_fanfare_timer == 2400) sfx_step = 1027;
        else if (sfx_fanfare_timer == 3600) sfx_step = 1371;

        sfx_phase = (sfx_phase + sfx_step) & 0xFFFF;
        sfx_val = (sfx_phase < 32768) ? sfx_amp : -sfx_amp;
      }

      sfx_amp -= sfx_decay;
      sample += sfx_val;
    }

    /* Clamp 16-bit signed audio range */
    if (sample > 32767) sample = 32767;
    else if (sample < -32768) sample = -32768;
    audio_mix_buf[i] = (short)sample;
  }

}

/* Refill according to queued bytes rather than video-frame frequency.
 * CRT syscalls copy accepted PCM synchronously into the kernel-owned ring.
 * Positive partial writes retain the suffix; zero/error returns defer it to
 * the next game iteration. The local byte budget bounds work even if hardware
 * consumes concurrently; sequentially consistent ordering needs no atomics
 * because only this game thread owns the mixer and pending block.
 */
void audio_service(struct GameWorld* w) {
  if (!audio_device_claimed || !w->sound_enabled) return;
  int buffered = audio_buffered();
  if (buffered < 0) return;

  while (buffered < AUDIO_TARGET_BYTES) {
    if (pending_samples == 0) {
      synthesize_block(w);
      pending_samples = AUDIO_BUF_SAMPLES;
      pending_offset = 0;
    }
    int bytes = pending_samples << 1;
    int budget = AUDIO_TARGET_BYTES - buffered;
    if (bytes > budget) bytes = budget;
    int written = audio_write(&audio_mix_buf[pending_offset], bytes);
    if (written <= 0) return;
    int samples = written >> 1;
    pending_offset += samples;
    pending_samples -= samples;
    buffered += written;
  }
}
