#include "realm.h"
#include "../crt/sys.h"

#define AUDIO_BUF_SAMPLES 500
static short audio_mix_buf[AUDIO_BUF_SAMPLES];

static int audio_device_claimed = 0;
static int current_sfx = 0;
static int sfx_sample_idx = 0;

/* Musical sequencer state. */
static int music_note_idx = 0;
static int music_sample_timer = 0;
static int music_phase_accum = 0;

#define MELODY_LENGTH 12

/* Medieval Dorian melody notes (pitch step increments per sample at 25 kHz). */
static int melody_pitches[MELODY_LENGTH] = {
  768, 914, 1027, 1153, 1371, 1153, 1027, 914, 768, 865, 914, 768
};

/* Initialize the hardware audio controller ring. */
void audio_init(void) {
  if (audio_open() == 0) {
    audio_device_claimed = 1;
  }
}

/* Gracefully release hardware audio device. */
void audio_shutdown(void) {
  if (audio_device_claimed) {
    audio_close();
    audio_device_claimed = 0;
  }
}

/* Queue a one-shot audio sound effect to play immediately. */
void audio_play_sfx(int sfx_id) {
  current_sfx = sfx_id;
  sfx_sample_idx = 0;
}

/* Synthesize and stream audio samples to keep hardware ring replenished. */
void audio_service(struct GameWorld* w) {
  if (!audio_device_claimed || !w->sound_enabled) return;

  int buffered = audio_buffered();
  if (buffered < 0 || buffered > 6000) return;

  /* Synthesize next audio block */
  for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
    int sample = 0;

    /* Background musical melody */
    music_sample_timer++;
    if (music_sample_timer >= 3000) { /* ~120 ms per note */
      music_sample_timer = 0;
      music_note_idx = (music_note_idx + 1) % MELODY_LENGTH;
    }

    int step = melody_pitches[music_note_idx];
    if (w->phase == PHASE_NIGHT) {
      /* Pitch shift up for nighttime battle tension */
      step = step * 5 / 4;
    }
    music_phase_accum = (music_phase_accum + step) & 0xFFFF;

    /* Triangle wave generator */
    int tri = (music_phase_accum < 32768) ? (music_phase_accum - 16384) : (49152 - music_phase_accum);
    sample += (tri / 16);

    /* Sound effect synthesis */
    if (current_sfx != 0) {
      sfx_sample_idx++;
      int sfx_val = 0;

      switch (current_sfx) {
        case SFX_WOOD_CHOP:
          if (sfx_sample_idx < 1200) {
            int decay = 1200 - sfx_sample_idx;
            sfx_val = (((sfx_sample_idx * 137) & 0x1FFF) - 0x0FFF) * decay / 1200;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_STONE_MINE:
          if (sfx_sample_idx < 1800) {
            int freq_acc = (sfx_sample_idx * 1200 * 65536 / 25000) & 0xFFFF;
            int ring = (freq_acc < 32768) ? 3000 : -3000;
            sfx_val = ring * (1800 - sfx_sample_idx) / 1800;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_ARROW_SHOOT:
          if (sfx_sample_idx < 1000) {
            sfx_val = (((sfx_sample_idx * 79) & 0x0FFF) - 0x07FF) * (1000 - sfx_sample_idx) / 1000;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_SWORD_HIT:
          if (sfx_sample_idx < 1600) {
            int metal = (((sfx_sample_idx * 880 * 65536 / 25000) & 0xFFFF) < 32768) ? 4000 : -4000;
            sfx_val = metal * (1600 - sfx_sample_idx) / 1600;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_BUILD_PLACE:
          if (sfx_sample_idx < 2500) {
            int drum = (((sfx_sample_idx * 140 * 65536 / 25000) & 0xFFFF) < 32768) ? 5000 : -5000;
            sfx_val = drum * (2500 - sfx_sample_idx) / 2500;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_BELL_DUSK:
          if (sfx_sample_idx < 6000) {
            int bell_acc = (sfx_sample_idx * 330 * 65536 / 25000) & 0xFFFF;
            int bell = (bell_acc < 32768) ? 6000 : -6000;
            sfx_val = bell * (6000 - sfx_sample_idx) / 6000;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_FANFARE_DAWN:
          if (sfx_sample_idx < 5000) {
            int fan_step = 262 * 65536 / 25000;
            if (sfx_sample_idx > 1200) fan_step = 330 * 65536 / 25000;
            if (sfx_sample_idx > 2400) fan_step = 392 * 65536 / 25000;
            if (sfx_sample_idx > 3600) fan_step = 523 * 65536 / 25000;
            int fan_acc = (sfx_sample_idx * fan_step) & 0xFFFF;
            sfx_val = ((fan_acc < 32768) ? 4500 : -4500);
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_GOLD_CHIME:
          if (sfx_sample_idx < 1500) {
            int chime_acc = (sfx_sample_idx * 1760 * 65536 / 25000) & 0xFFFF;
            sfx_val = ((chime_acc < 32768) ? 4000 : -4000) * (1500 - sfx_sample_idx) / 1500;
          } else {
            current_sfx = 0;
          }
          break;

        case SFX_DEFEAT:
          if (sfx_sample_idx < 7000) {
            int low_acc = (sfx_sample_idx * 110 * 65536 / 25000) & 0xFFFF;
            sfx_val = ((low_acc < 32768) ? 5000 : -5000) * (7000 - sfx_sample_idx) / 7000;
          } else {
            current_sfx = 0;
          }
          break;

        default:
          current_sfx = 0;
          break;
      }

      sample += sfx_val;
    }

    /* Clamp 16-bit signed audio range */
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    audio_mix_buf[i] = (short)sample;
  }

  audio_write(audio_mix_buf, AUDIO_BUF_SAMPLES * 2);
}
