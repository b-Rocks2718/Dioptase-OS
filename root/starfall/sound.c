#include "sound.h"

#include "../crt/sys.h"

/* docs/mem_map.md fixes mono PCM at 25 kHz and 16 bits per sample. */
#define AUDIO_BLOCK_SAMPLES 512
/* bcc's global array parser requires the literal 512-sample byte capacity. */
#define AUDIO_BLOCK_BYTES 1024
#define AUDIO_TARGET_BYTES 4096
#define AUDIO_REFILL_LIMIT 6

static int audio_owned;
static unsigned char pcm[AUDIO_BLOCK_BYTES];
static int pcm_offset;
static int pcm_pending;
static unsigned melody_phase;
static unsigned bass_phase;
static unsigned effect_phase;
static unsigned noise_state = 0xC001D00Du;
static int effect_length;
static int effect_tone;
static unsigned audio_sample_count;
static unsigned percussion_phase;

/* The phase steps correspond to a minor-key melody at the documented rate. */
static unsigned melody_steps[16] = {
  684, 0, 815, 684, 1026, 0, 913, 815,
  684, 0, 544, 611, 684, 0, 544, 456
};
static unsigned bass_steps[16] = {
  342, 342, 342, 342, 272, 272, 272, 272,
  228, 228, 228, 228, 305, 305, 305, 305
};

/* Select the most urgent new effect without interrupting game simulation. */
static void select_effect(int events) {
  if (events & SOUND_PULSE) {
    effect_length = 2800;
    effect_tone = 490;
  } else if (events & SOUND_DAMAGE) {
    effect_length = 1400;
    effect_tone = 211;
  } else if (events & SOUND_WAVE) {
    effect_length = 2300;
    effect_tone = 850;
  } else if (events & SOUND_KILL) {
    effect_length = 900;
    effect_tone = 690;
  } else if (events & SOUND_PICKUP) {
    effect_length = 900;
    effect_tone = 1100;
  } else if ((events & SOUND_FIRE) && effect_length < 180) {
    effect_length = 320;
    effect_tone = 1700;
  }
}

/* Produce one little-endian PCM block with two voices and simple percussion. */
static void synthesize_block(struct Game* game) {
  int beat = (int)((game->frame / 10) % 16);
  unsigned melody_step = melody_steps[beat];
  unsigned bass_step = bass_steps[beat];
  for (int sample_index = 0; sample_index < AUDIO_BLOCK_SAMPLES;
       ++sample_index) {
    int sample = 0;
    melody_phase = (melody_phase + melody_step) & 0xFFFFu;
    bass_phase = (bass_phase + bass_step) & 0xFFFFu;
    if (melody_step != 0) {
      sample += (melody_phase & 0x8000u) ? 1450 : -1450;
    }
    sample += (bass_phase & 0x8000u) ? 1500 : -1500;
    if ((beat & 1) == 0 && percussion_phase < 430) {
      if ((audio_sample_count & 3u) == 0) {
        /* Xorshift keeps the percussion noisy without software multiply. */
        noise_state ^= noise_state << 13;
        noise_state ^= noise_state >> 17;
        noise_state ^= noise_state << 5;
      }
      sample += (noise_state & 0x80000000u) ? 750 : -750;
    }
    if (effect_length > 0) {
      effect_phase = (effect_phase + (unsigned)effect_tone) & 0xFFFFu;
      sample += (effect_phase & 0x8000u) ? 3300 : -3300;
      effect_length--;
    }
    /* Conversion through unsigned is defined modulo 2^16 for negative PCM. */
    unsigned encoded = (unsigned)sample & 0xFFFFu;
    pcm[sample_index * 2] = (unsigned char)(encoded & 255u);
    pcm[sample_index * 2 + 1] = (unsigned char)(encoded >> 8);
    audio_sample_count++;
    /* Match unsigned sample-count wrap as well as the 2500-sample beat. */
    if (audio_sample_count == 0 || ++percussion_phase == 2500) {
      percussion_phase = 0;
    }
  }
  pcm_offset = 0;
  pcm_pending = AUDIO_BLOCK_BYTES;
}

/* Optional audio failure leaves the visual game fully playable. */
void sound_init(void) {
  audio_owned = audio_open() == 0;
  pcm_offset = 0;
  pcm_pending = 0;
  melody_phase = 0;
  bass_phase = 0;
  effect_phase = 0;
  effect_length = 0;
  audio_sample_count = 0;
  percussion_phase = 0;
}

/* Refill only a modest ring prefix so sound follows gameplay state promptly. */
void sound_tick(struct Game* game) {
  if (!audio_owned) return;
  select_effect(game->sound_events);
  int buffered = audio_buffered();
  if (buffered < 0) {
    audio_owned = 0;
    return;
  }
  for (int block = 0; block < AUDIO_REFILL_LIMIT &&
                      buffered < AUDIO_TARGET_BYTES; ++block) {
    if (pcm_pending == 0) synthesize_block(game);
    int written = audio_write(pcm + pcm_offset, (unsigned)pcm_pending);
    if (written <= 0) break;
    pcm_offset += written;
    pcm_pending -= written;
    buffered += written;
  }
}

/* Ownership is process-scoped, but an explicit close stops playback promptly. */
void sound_shutdown(void) {
  if (audio_owned) {
    audio_close();
    audio_owned = 0;
  }
}
