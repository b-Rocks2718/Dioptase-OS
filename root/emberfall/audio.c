/* Integer chiptune mixer for the 25 kHz mono PCM device.
 *
 * Periods are computed once per tick; the 840-sample loop uses only adds,
 * compares, and wrapped phase counters so no division runs per sample.
 * Drums (kick on quarters, hats on offbeats) join the lead and bass loop.
 */

#include "audio.h"

#include "../crt/sys.h"

#define SAMPLE_RATE 25000

/* A-minor adventure loop: lead eighth notes with a driving bass line.
 * Frequencies in Hz; 0 rests. Eighth notes at 140 bpm. */
static int lead_notes[32] = {
  440, 0, 523, 0, 659, 0, 523, 659,
  440, 0, 392, 440, 330, 0, 392, 0,
  440, 0, 523, 659, 784, 0, 659, 523,
  659, 523, 440, 392, 330, 392, 440, 0
};

static int bass_notes[32] = {
  110, 110, 131, 131, 98, 98, 82, 82,
  110, 110, 131, 131, 98, 98, 82, 82,
  110, 110, 131, 131, 165, 165, 131, 131,
  110, 110, 98, 98, 82, 82, 82, 82
};

static int audio_owned = 0;
static int music_step = 0;
static int music_tick = 0;

/* Wrapped phase positions, advanced by one per sample without modulo. */
static int lead_pos = 0;
static int bass_pos = 0;
static int pos_shoot = 0;
static int pos_pickup = 0;
static int pos_levelup = 0;
static int pos_hurt = 0;
static int pos_boss = 0;
static int pos_over = 0;
static int pos_ui = 0;
static int pos_kick = 0;

/* One-shot effect voices with ticks remaining. */
static int fx_shoot = 0;
static int fx_hit = 0;
static int fx_explode = 0;
static int fx_pickup = 0;
static int fx_levelup = 0;
static int fx_hurt = 0;
static int fx_boss = 0;
static int fx_over = 0;
static int fx_ui = 0;
static int drum_kick = 0;
static int drum_hat = 0;
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

/* 16-bit LFSR noise, -1 or 1. */
static int noise_bit(void) {
  int bit = 0;
  noise_state ^= noise_state << 7;
  noise_state ^= noise_state >> 9;
  noise_state ^= noise_state << 8;
  bit = noise_state & 1;
  return bit ? 1 : -1;
}

/* Advance a wrapped position by one sample. */
static void step_pos(int* pos, int period) {
  int p = *pos + 1;
  if (p >= period) p = 0;
  *pos = p;
}

/* Mix one music-plus-effects slice into the ring. */
void audio_tick(int sound_bits, int muted, int phase_playing) {
  static short pcm[840];
  int i = 0;
  int lead = 0;
  int bass = 0;
  int lead_period = 1;
  int bass_period = 1;
  int bass_half = 0;
  int lead_gain = 900;
  int shoot_period = 1;
  int shoot_half = 0;
  int pickup_period = 1;
  int pickup_half = 0;
  int levelup_period = 1;
  int levelup_half = 0;
  int hurt_period = 1;
  int hurt_half = 0;
  int boss_period = 1;
  int boss_half = 0;
  int over_period = 1;
  int over_half = 0;
  int ui_period = 1;
  int ui_half = 0;
  int kick_period = 1;
  int kick_half = 0;
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
  /* Eighth notes at 140 bpm: one step every 11 ticks at 30 Hz. */
  if (!muted && phase_playing) {
    music_tick++;
    if (music_tick >= 11) {
      music_tick = 0;
      music_step++;
      if (music_step >= 32) music_step = 0;
      /* Kick on quarters, hats on the offbeats. */
      if (music_step == 0 || music_step == 8 || music_step == 16 ||
          music_step == 24) {
        drum_kick = 3;
      }
      if (music_step == 4 || music_step == 12 || music_step == 20 ||
          music_step == 28) {
        drum_hat = 2;
      }
    }
  }
  lead = lead_notes[music_step];
  bass = bass_notes[music_step];
  if (lead > 0) lead_period = SAMPLE_RATE / lead;
  if (bass > 0) {
    bass_period = SAMPLE_RATE / bass;
    bass_half = bass_period / 2;
  }
  /* Accent the downbeats. */
  if (music_step == 0 || music_step == 8 || music_step == 16 ||
      music_step == 24) {
    lead_gain = 1150;
  }
  if (lead_pos >= lead_period) lead_pos = 0;
  if (bass_pos >= bass_period) bass_pos = 0;
  /* Per-tick effect periods; the sample loop only wraps positions. */
  if (fx_shoot > 0) {
    shoot_period = SAMPLE_RATE / (880 + fx_shoot * 40);
    shoot_half = shoot_period / 2;
    if (pos_shoot >= shoot_period) pos_shoot = 0;
  }
  if (fx_pickup > 0) {
    pickup_period = SAMPLE_RATE / (660 + (10 - fx_pickup) * 90);
    pickup_half = pickup_period / 2;
    if (pos_pickup >= pickup_period) pos_pickup = 0;
  }
  if (fx_levelup > 0) {
    int f = 523 + ((30 - fx_levelup) / 6) * 131;
    levelup_period = SAMPLE_RATE / f;
    levelup_half = levelup_period / 2;
    if (pos_levelup >= levelup_period) pos_levelup = 0;
  }
  if (fx_hurt > 0) {
    hurt_period = SAMPLE_RATE / 110;
    hurt_half = hurt_period / 2;
    if (pos_hurt >= hurt_period) pos_hurt = 0;
  }
  if (fx_boss > 0) {
    boss_period = SAMPLE_RATE / 65;
    boss_half = boss_period / 2;
    if (pos_boss >= boss_period) pos_boss = 0;
  }
  if (fx_over > 0) {
    int f = 330 - (60 - fx_over) * 4;
    if (f < 55) f = 55;
    over_period = SAMPLE_RATE / f;
    over_half = over_period / 2;
    if (pos_over >= over_period) pos_over = 0;
  }
  if (fx_ui > 0) {
    ui_period = SAMPLE_RATE / 700;
    ui_half = ui_period / 2;
    if (pos_ui >= ui_period) pos_ui = 0;
  }
  if (drum_kick > 0) {
    kick_period = SAMPLE_RATE / 55;
    kick_half = kick_period / 2;
    if (pos_kick >= kick_period) pos_kick = 0;
  }
  while (i < 840) {
    int s = 0;
    if (!muted && phase_playing) {
      if (lead > 0) {
        s += (lead_pos < lead_period / 2 ? 1 : -1) * lead_gain;
        step_pos(&lead_pos, lead_period);
      }
      if (bass > 0) {
        int p = bass_pos;
        int v = 0;
        if (p < bass_half) v = p * 2 - bass_half;
        else v = bass_half * 3 - p * 2;
        s += v * 7;
        step_pos(&bass_pos, bass_period);
      }
      if (drum_kick > 0) {
        int p = pos_kick;
        int v = 0;
        if (p < kick_half) v = p * 2 - kick_half;
        else v = kick_half * 3 - p * 2;
        s += v * drum_kick * 4;
        step_pos(&pos_kick, kick_period);
      }
      if (drum_hat > 0) {
        s += noise_bit() * drum_hat * 260;
      }
    }
    if (fx_shoot > 0) {
      s += (pos_shoot < shoot_half ? 1 : -1) * fx_shoot * 60;
      step_pos(&pos_shoot, shoot_period);
    }
    if (fx_hit > 0) {
      s += noise_bit() * fx_hit * 120;
    }
    if (fx_explode > 0) {
      s += noise_bit() * fx_explode * 160;
    }
    if (fx_pickup > 0) {
      s += (pos_pickup < pickup_half ? 1 : -1) * fx_pickup * 90;
      step_pos(&pos_pickup, pickup_period);
    }
    if (fx_levelup > 0) {
      s += (pos_levelup < levelup_half ? 1 : -1) * 1100;
      step_pos(&pos_levelup, levelup_period);
    }
    if (fx_hurt > 0) {
      s += (pos_hurt < hurt_half ? 1 : -1) * fx_hurt * 130;
      step_pos(&pos_hurt, hurt_period);
    }
    if (fx_boss > 0) {
      s += (pos_boss < boss_half ? 1 : -1) * 1400;
      s += noise_bit() * 400;
      step_pos(&pos_boss, boss_period);
    }
    if (fx_over > 0) {
      s += (pos_over < over_half ? 1 : -1) * 1200;
      step_pos(&pos_over, over_period);
    }
    if (fx_ui > 0) {
      s += (pos_ui < ui_half ? 1 : -1) * fx_ui * 120;
      step_pos(&pos_ui, ui_period);
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
  if (drum_kick > 0) drum_kick--;
  if (drum_hat > 0) drum_hat--;
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
