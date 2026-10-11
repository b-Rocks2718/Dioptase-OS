/* Integer chiptune mixer for the 25 kHz mono PCM device. */

#include "audio.h"

#include "../crt/sys.h"

#define SAMPLE_RATE 25000
#define SLICE_SAMPLES 840
#define NOTE_COUNT 32

/* A-minor adventure loop: lead eighth notes with a driving bass line.
 * Frequencies in Hz; 0 rests. 16 steps at 140 bpm eighth notes. */
static int lead_notes[32] = {
  440, 0, 523, 0, 659, 0, 523, 0,
  440, 0, 392, 0, 330, 0, 392, 0,
  440, 0, 523, 0, 659, 0, 784, 0,
  659, 523, 440, 392, 330, 0, 0, 0
};

static int bass_notes[32] = {
  110, 110, 110, 110, 131, 131, 131, 131,
  110, 110, 110, 110, 98, 98, 82, 82,
  110, 110, 110, 110, 131, 131, 165, 165,
  131, 131, 110, 110, 82, 82, 82, 82
};

static int audio_owned = 0;
static int music_step = 0;
static int music_tick = 0;
static int lead_phase = 0;
static int bass_phase = 0;

/* One-shot effect voices with frames remaining. */
static int fx_shoot = 0;
static int fx_hit = 0;
static int fx_explode = 0;
static int fx_pickup = 0;
static int fx_levelup = 0;
static int fx_hurt = 0;
static int fx_boss = 0;
static int fx_over = 0;
static int fx_ui = 0;
static int noise_state = 0xACE1u;

/* Claim the PCM device for this process. */
void audio_init(void) {
  if (audio_open() == 0) audio_owned = 1;
}

/* Release the PCM device. */
void audio_shutdown(void) {
  if (audio_owned) {
    audio_close();
    audio_owned = 0;
  }
}

/* Square wave at freq Hz, phase in 0..999. */
static int square_wave(int phase, int freq) {
  int period = SAMPLE_RATE / freq;
  int half = period / 2;
  int p = phase % period;
  return p < half ? 1 : -1;
}

/* Triangle-ish wave via a wrapped ramp, for softer bass. */
static int tri_wave(int phase, int freq) {
  int period = SAMPLE_RATE / freq;
  int p = 0;
  if (period <= 0) return 0;
  p = phase % period;
  if (p < period / 2) return p * 2 - period / 2;
  return period * 3 / 2 - p * 2;
}

/* 16-bit LFSR noise, -1 or 1. */
static int noise_bit(void) {
  int bit = 0;
  noise_state ^= noise_state << 7;
  noise_state ^= noise_state >> 9;
  noise_state ^= noise_state << 8;
  bit = noise_state & 1;
  return bit ? 1 : -1;
}

/* Mix one music-plus-effects slice into the ring. */
void audio_tick(int sound_bits, int muted, int phase_playing) {
  static short pcm[840];
  int i = 0;
  if (!audio_owned) return;
  if (sound_bits & 1) fx_shoot = 6;
  if (sound_bits & 2) fx_hit = 8;
  if (sound_bits & 4) fx_explode = 22;
  if (sound_bits & 8) fx_pickup = 10;
  if (sound_bits & 16) fx_levelup = 30;
  if (sound_bits & 32) fx_hurt = 16;
  if (sound_bits & 64) fx_boss = 40;
  if (sound_bits & 128) fx_over = 60;
  if (sound_bits & 256) fx_ui = 5;
  /* Eighth notes at 140 bpm: one step every ~10.7 ticks at 30 Hz. */
  if (!muted && phase_playing) {
    music_tick++;
    if (music_tick >= 11) {
      music_tick = 0;
      music_step++;
      if (music_step >= 32) music_step = 0;
    }
  }
  while (i < 840) {
    int s = 0;
    int lead = lead_notes[music_step];
    int bass = bass_notes[music_step];
    if (!muted && phase_playing) {
      if (lead > 0) s += square_wave(lead_phase, lead) * 900;
      if (bass > 0) s += tri_wave(bass_phase, bass) * 14;
      lead_phase++;
      bass_phase++;
    }
    if (fx_shoot > 0) {
      s += square_wave(i * 3, 880 + fx_shoot * 40) * fx_shoot * 60;
    }
    if (fx_hit > 0) {
      s += noise_bit() * fx_hit * 120;
    }
    if (fx_explode > 0) {
      s += noise_bit() * fx_explode * 160;
    }
    if (fx_pickup > 0) {
      s += square_wave(i * 2, 660 + (10 - fx_pickup) * 90) * fx_pickup * 90;
    }
    if (fx_levelup > 0) {
      int f = 523 + ((30 - fx_levelup) / 6) * 131;
      s += square_wave(i, f) * 1100;
    }
    if (fx_hurt > 0) {
      s += square_wave(i, 110) * fx_hurt * 130;
    }
    if (fx_boss > 0) {
      s += square_wave(i / 2, 65) * 1400;
      s += noise_bit() * 400;
    }
    if (fx_over > 0) {
      int f = 330 - (60 - fx_over) * 4;
      if (f < 55) f = 55;
      s += square_wave(i, f) * 1200;
    }
    if (fx_ui > 0) {
      s += square_wave(i, 700) * fx_ui * 120;
    }
    if (s > 12000) s = 12000;
    if (s < -12000) s = -12000;
    pcm[i] = (short)s;
    i++;
  }
  if (fx_shoot > 0) fx_shoot--;
  if (fx_hit > 0) fx_hit--;
  if (fx_explode > 0) fx_explode--;
  if (fx_pickup > 0) fx_pickup--;
  if (fx_levelup > 0) fx_levelup--;
  if (fx_hurt > 0) fx_hurt--;
  if (fx_boss > 0) fx_boss--;
  if (fx_over > 0) fx_over--;
  if (fx_ui > 0) fx_ui--;
  /* Non-blocking refill: accept partial writes when the ring is tight. */
  {
    int off = 0;
    int guard = 0;
    while (off < 1680 && guard < 8) {
      int n = audio_write((void*)(((char*)pcm) + off), 1680 - off);
      if (n <= 0) break;
      off += n;
      guard++;
    }
  }
}
