#include "lantern.h"
#include "../crt/sys.h"

static int audio_on;
static unsigned mel_phase;
static unsigned drone_phase;
static unsigned noise = 1;
static unsigned sfx_phase;
static int note_pos;
static int note_left;
static int sfx_kind;
static int sfx_left;
static short pcm[128];

/* Phase steps for a 25 kHz square-wave minor loop. Precomputed so playback
 * does not multiply: step = hz * 65536 / 25000. */
static int note_step[16] = {
  288, 343, 385, 432, 385, 343, 288, 257,
  288, 343, 432, 385, 343, 288, 257, 288
};

/* Unsigned xorshift. The low bits are a cheap noise source for impacts. */
static int noise_sample(void) {
  noise ^= noise << 13;
  noise ^= noise >> 17;
  noise ^= noise << 5;
  return (int)((noise >> 17) & 1023) - 512;
}

/* One sound-effect sample, mixed over the drone. */
static int sfx_voice(void) {
  int wave;
  sfx_phase += 1;
  if (sfx_kind == SFX_SHOT || sfx_kind == SFX_DOOR || sfx_kind == SFX_STEP) {
    wave = noise_sample();
    if (sfx_kind == SFX_STEP) wave = wave >> 2;
    return wave;
  }
  if (sfx_kind == SFX_HURT || sfx_kind == SFX_DIE) {
    sfx_phase += 90;
  } else if (sfx_kind == SFX_PICKUP) {
    sfx_phase += (sfx_left > 1200) ? 400 : 700;
  } else if (sfx_kind == SFX_CORE || sfx_kind == SFX_WIN) {
    sfx_phase += 200 + ((sfx_phase >> 8) & 255);
  } else {
    sfx_phase += 520;
  }
  if (sfx_phase & 0x8000) return 5000;
  return -1800;
}

/* Next music sample, with the active effect added and clamped. */
static short next_sample(void) {
  int sample = 0;
  int step = note_step[note_pos];

  if ((mel_phase & 0xE000) == 0xE000) sample += 1800;
  else sample -= 500;
  mel_phase += (unsigned)step;

  if (drone_phase & 0x8000) sample += 700;
  else sample -= 700;
  drone_phase += (unsigned)(step >> 1);

  note_left++;
  if (note_left >= 6500) {
    note_left = 0;
    note_pos++;
    if (note_pos >= 16) note_pos = 0;
  }

  if (sfx_left > 0) {
    sample += sfx_voice();
    sfx_left--;
  }
  if (sample > 30000) sample = 30000;
  if (sample < -30000) sample = -30000;
  return (short)sample;
}

void audio_start(void) {
  if (lantern_bench) return;
  if (audio_open() != 0) {
    audio_on = 0;
    return;
  }
  audio_on = 1;
  note_pos = 0;
  note_left = 0;
}

void audio_pump(void) {
  int guard = 0;
  if (!audio_on) return;
  while (guard < 20) {
    int buffered = audio_buffered();
    int i;
    if (buffered < 0 || buffered > 12000) return;
    for (i = 0; i < 128; i++) {
      pcm[i] = next_sample();
    }
    if (audio_write(pcm, 256) < 256) return;
    guard++;
  }
}

void sfx_play(int kind) {
  if (!audio_on) return;
  if (kind == SFX_STEP && sfx_left > 0) return;
  sfx_kind = kind;
  sfx_phase = 0;
  if (kind == SFX_STEP) sfx_left = 350;
  else if (kind == SFX_SHOT) sfx_left = 1600;
  else if (kind == SFX_WIN || kind == SFX_DIE) sfx_left = 7000;
  else sfx_left = 2200;
}

void audio_stop(void) {
  if (audio_on) {
    audio_close();
  }
  audio_on = 0;
}
