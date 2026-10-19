#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "retake.h"
#include "pak.h"

/*
 * Sound: a synthesizer running in its own process (the approach DELVE
 * proved). audio_start() forks a child that owns the PCM device
 * (docs/syscalls.md, "Audio": signed 16-bit mono at 25 kHz from a ~0.33 s
 * ring), so music never stutters while the game loads a room or shows a
 * card, and the scheduler can run it on another core. The game talks to it
 * over a pipe with one-byte commands:
 *
 *   1..63      play sound effect N (SFX_* in retake.h)
 *   64 + song  switch music (64 + MUSIC_NONE silences it)
 *   CMD_ALIVE  heartbeat; the child exits if it stops for 60 seconds
 *   CMD_QUIT   release the device and exit
 *
 * Synth: 8 voices (6 music channels, 2 effects). A voice is a 32-bit phase
 * accumulator stepping through a 256-entry waveform pre-scaled to 16 volume
 * levels at startup, or an LFSR for noise, so mixing needs no multiplies.
 * Envelopes, sweeps and the sequencer run at a 100 Hz control rate
 * (250 samples per control tick).
 *
 * Process state: the child inherits the read-only pak mapping, so it must be
 * forked after pak_open() and, to keep fork cheap, before the large room
 * buffers are allocated.
 */

#define CMD_MUSIC 64
#define CMD_ALIVE 0xFE
#define CMD_QUIT 0xFF
/* Control ticks (60 s of audio) without a heartbeat before the child gives
 * up. It only exists to silence an orphaned synthesizer after a crash. It
 * used to be 5 s, but with wall-clock playback a slow emulator can spend that
 * long loading a room, and the music then stayed off for the session. */
#define HEARTBEAT_TIMEOUT 6000
#define TICK_SAMPLES 250
#define TARGET_BYTES 3000         /* ~60 ms queued in the device ring after a refill */
#define LOW_BYTES 1500            /* refill once the queue drains to ~30 ms */
#define NAP_BYTES 300             /* bytes of queue spent per jiffy of nap (~6 ms of audio) */
#define VOICES 8
#define MUSIC_VOICES 6
#define LEVELS 16
#define WAVE_NOISE 99

static int cmd_fd = -1;           /* game side: pipe write end */
static int audio_ok;

/* ---- game side ---- */

static void send(int byte) {
  if (cmd_fd < 0) return;
  unsigned char b = (unsigned char)byte;
  write(cmd_fd, &b, 1);
}

void audio_sfx(int id) {
  send(id);
}

void audio_music(int song) {
  send(CMD_MUSIC + song);
}

void audio_heartbeat(void) {
  send(CMD_ALIVE);
}

void audio_stop(void) {
  send(CMD_QUIT);
  if (cmd_fd >= 0) close(cmd_fd);
  cmd_fd = -1;
}

/* ---- synthesizer (child process) ---- */

struct Voice {
  unsigned phase;
  unsigned inc;
  unsigned base_inc;
  short* table;          /* current level's 256 samples, or 0 if silent */
  int wave;
  int level;
  int peak;
  int decay;             /* control ticks per level step down, 0 = hold */
  int sustain;
  int release;
  int env_timer;
  int releasing;
  int sweep;             /* added to base_inc each control tick */
  int vib_depth;
  int vib_rate;
  int vib_phase;
  int ticks_left;        /* effects: control ticks until release, -1 = none */
  unsigned lfsr;
};

struct Inst {
  unsigned char wave;
  unsigned char peak;
  unsigned char decay;
  unsigned char sustain;
  unsigned char release;
  unsigned char vib_depth;
  unsigned char vib_rate;
  unsigned char pad;
};

static short* wave_levels;        /* [wave][level][256] */
static int wave_count;
static unsigned* note_incs;       /* [128] */
static struct Voice voices[8];
static int mix[250];
static short out_buf[250];

/* song state */
static unsigned char* song;
static int song_rows;
static int song_chans;
static int song_tempo;
static unsigned char* song_inst;
static unsigned char* song_notes;
static int song_row;
static int song_timer;

/* effects queue: each effect step is a voice program */
struct SfxStep {
  int wave;
  int note;
  int sweep;
  int level;
  int decay;
  int ticks;
  int delay;             /* control ticks after the effect starts */
};

static struct SfxStep sfx_pending[8];
static int sfx_pending_count;
static int next_sfx_voice;

static void voice_set_level(struct Voice* v, int level) {
  if (level < 0) level = 0;
  if (level > 15) level = 15;
  v->level = level;
  if (v->wave == WAVE_NOISE) {
    v->table = level ? (short*)1 : 0;
    return;
  }
  v->table = level ? wave_levels + ((v->wave * LEVELS + level) << 8) : 0;
}

static void voice_note(struct Voice* v, struct Inst* in, int note) {
  v->wave = in->wave;
  if (v->wave == WAVE_NOISE && v->lfsr == 0) v->lfsr = 0xACE1u;   /* music voices start unseeded */
  v->peak = in->peak;
  v->decay = in->decay;
  v->sustain = in->sustain;
  v->release = in->release;
  v->vib_depth = in->vib_depth;
  v->vib_rate = in->vib_rate;
  v->base_inc = note_incs[note & 127];
  v->inc = v->base_inc;
  v->env_timer = 0;
  v->releasing = 0;
  v->sweep = 0;
  v->ticks_left = -1;
  voice_set_level(v, in->peak);
}

static void voice_control(struct Voice* v) {
  if (!v->table) return;
  if (v->ticks_left > 0 && --v->ticks_left == 0) v->releasing = 1;
  int rate = v->releasing ? v->release : v->decay;
  int floor = v->releasing ? 0 : v->sustain;
  if (rate > 0 && v->level > floor) {
    if (++v->env_timer >= rate) {
      v->env_timer = 0;
      voice_set_level(v, v->level - 1);
    }
  } else if (v->releasing && rate == 0) {
    voice_set_level(v, 0);
  }
  if (v->sweep) v->base_inc += (unsigned)v->sweep;
  v->inc = v->base_inc;
  if (v->vib_depth) {
    v->vib_phase += v->vib_rate;
    int tri = (v->vib_phase >> 2) & 31;
    tri = tri < 16 ? tri - 8 : 24 - tri;
    v->inc = v->base_inc + (unsigned)((int)(v->base_inc >> 10) * tri * v->vib_depth >> 3);
  }
}

static void sequencer_tick(void) {
  if (!song) return;
  if (--song_timer > 0) return;
  song_timer = song_tempo;
  unsigned char* row = song_notes + song_row * song_chans;
  for (int c = 0; c < song_chans && c < MUSIC_VOICES; c++) {
    int n = row[c];
    if (n == 0) continue;
    struct Voice* v = &voices[c];
    if (n == 1) {
      v->releasing = 1;
      continue;
    }
    struct Inst* in = (struct Inst*)(g_pak + pak_header(PAKH_INST_OFF)) + song_inst[c];
    voice_note(v, in, n);
  }
  if (++song_row >= song_rows) song_row = 0;
}

static void start_song(int index) {
  for (int c = 0; c < MUSIC_VOICES; c++) voice_set_level(&voices[c], 0);
  if (index < 0 || index >= (int)pak_header(PAKH_SONG_COUNT)) {
    song = 0;
    return;
  }
  unsigned* offs = (unsigned*)(g_pak + pak_header(PAKH_SONG_OFF));
  song = g_pak + offs[index];
  unsigned short* head = (unsigned short*)song;
  song_rows = head[0];
  song_chans = head[1];
  song_tempo = head[2];
  song_inst = song + 8;
  song_notes = song_inst + ((song_chans + 3) & ~3);
  song_row = 0;
  song_timer = 1;
}

/* Effect programs: up to four steps each (wave, MIDI note, sweep per tick,
 * level, decay ticks per level, length in ticks, start delay). */
static void sfx_add(int wave, int note, int sweep, int level, int decay, int ticks, int delay) {
  if (sfx_pending_count >= 8) return;
  struct SfxStep* s = &sfx_pending[sfx_pending_count++];
  s->wave = wave;
  s->note = note;
  s->sweep = sweep;
  s->level = level;
  s->decay = decay;
  s->ticks = ticks;
  s->delay = delay;
}

#define W_SQUARE 0
#define W_PULSE 1
#define W_TRI 2
#define W_SAW 3
#define W_PIANO 4
#define W_SINE 6

static void sfx_start(int id) {
  switch (id) {
    case SFX_JUMP: sfx_add(W_PULSE, 62, 900000, 8, 2, 7, 0); break;
    case SFX_LAND: sfx_add(WAVE_NOISE, 30, 0, 6, 1, 4, 0); break;
    case SFX_CLAP:
      sfx_add(WAVE_NOISE, 90, 0, 13, 1, 4, 0);
      sfx_add(W_SQUARE, 96, -2000000, 9, 1, 2, 0);
      break;
    case SFX_HIT:
      sfx_add(WAVE_NOISE, 60, 0, 12, 1, 6, 0);
      sfx_add(W_SQUARE, 48, -400000, 10, 2, 8, 0);
      break;
    case SFX_KILL:
      sfx_add(W_SQUARE, 72, -1500000, 11, 2, 12, 0);
      sfx_add(WAVE_NOISE, 50, 0, 10, 2, 10, 2);
      break;
    case SFX_COIN:
      sfx_add(W_SQUARE, 88, 0, 9, 2, 5, 0);
      sfx_add(W_SQUARE, 95, 0, 9, 3, 12, 5);
      break;
    case SFX_PICKUP:
      sfx_add(W_SINE, 72, 0, 12, 4, 8, 0);
      sfx_add(W_SINE, 76, 0, 12, 4, 8, 6);
      sfx_add(W_SINE, 79, 0, 12, 4, 8, 12);
      sfx_add(W_SINE, 84, 0, 13, 6, 24, 18);
      break;
    case SFX_HURT: sfx_add(W_SQUARE, 64, -700000, 12, 2, 16, 0); break;
    case SFX_HAZARD:
      sfx_add(WAVE_NOISE, 40, 0, 13, 2, 10, 0);
      sfx_add(W_TRI, 50, -600000, 13, 3, 20, 0);
      break;
    case SFX_DEATH:   /* the sad trombone: wah, wah, wah, waaah */
      sfx_add(W_SAW, 67, 0, 11, 6, 14, 0);
      sfx_add(W_SAW, 66, 0, 11, 6, 14, 18);
      sfx_add(W_SAW, 65, 0, 11, 6, 14, 36);
      sfx_add(W_SAW, 64, -6000, 12, 9, 60, 54);
      break;
    case SFX_REC_START:
      sfx_add(WAVE_NOISE, 90, 0, 12, 1, 3, 0);
      sfx_add(W_PULSE, 48, 400000, 7, 0, 18, 2);
      break;
    case SFX_REC_STOP:
      sfx_add(WAVE_NOISE, 90, 0, 13, 1, 4, 0);
      sfx_add(W_SQUARE, 84, -1200000, 9, 1, 3, 0);
      break;
    case SFX_JUMPCUT:
      sfx_add(WAVE_NOISE, 70, 0, 11, 1, 6, 0);
      sfx_add(W_SAW, 60, 3000000, 10, 1, 8, 0);
      break;
    case SFX_SPRING: sfx_add(W_TRI, 55, 1200000, 13, 3, 18, 0); break;
    case SFX_GATE: sfx_add(W_SAW, 36, 40000, 11, 3, 20, 0); break;
    case SFX_SAVE:
      sfx_add(W_SINE, 79, 0, 12, 6, 16, 0);
      sfx_add(W_SINE, 84, 0, 12, 6, 16, 10);
      sfx_add(W_SINE, 88, 0, 12, 8, 30, 20);
      break;
    case SFX_CARD: sfx_add(WAVE_NOISE, 80, 0, 8, 1, 2, 0); break;
    case SFX_SELECT: sfx_add(W_SQUARE, 84, 0, 8, 1, 3, 0); break;
    default: break;
  }
}

static void sfx_tick(void) {
  for (int i = 0; i < sfx_pending_count;) {
    struct SfxStep* s = &sfx_pending[i];
    if (s->delay > 0) {
      s->delay--;
      i++;
      continue;
    }
    struct Voice* v = &voices[MUSIC_VOICES + next_sfx_voice];
    next_sfx_voice ^= 1;
    v->wave = s->wave;
    v->base_inc = note_incs[s->note & 127];
    v->inc = v->base_inc;
    v->sweep = s->sweep;
    v->decay = s->decay;
    v->sustain = 0;
    v->release = 1;
    v->env_timer = 0;
    v->releasing = 0;
    v->vib_depth = 0;
    v->ticks_left = s->ticks;
    v->lfsr = 0xACE1u;
    voice_set_level(v, s->level);
    sfx_pending[i] = sfx_pending[--sfx_pending_count];
  }
}

/* Mix one control tick (250 samples) into out_buf. */
static void mix_tick(void) {
  for (int i = 0; i < TICK_SAMPLES; i++) mix[i] = 0;
  for (int k = 0; k < VOICES; k++) {
    struct Voice* v = &voices[k];
    if (!v->table) continue;
    unsigned ph = v->phase;
    unsigned inc = v->inc;
    if (v->wave == WAVE_NOISE) {
      /* noise: a new random sample each time the phase wraps a 1/16 step */
      unsigned lfsr = v->lfsr;
      int amp = v->level << 7;
      int cur = (lfsr & 1) ? amp : -amp;
      for (int i = 0; i < TICK_SAMPLES; i++) {
        unsigned nph = ph + inc;
        if ((nph >> 28) != (ph >> 28)) {
          lfsr = (lfsr >> 1) ^ (unsigned)(-(int)(lfsr & 1) & 0xB400u);
          cur = (lfsr & 1) ? amp : -amp;
        }
        ph = nph;
        mix[i] += cur;
      }
      v->lfsr = lfsr;
    } else {
      short* t = v->table;
      for (int i = 0; i < TICK_SAMPLES; i++) {
        mix[i] += t[ph >> 24];
        ph += inc;
      }
    }
    v->phase = ph;
  }
  for (int i = 0; i < TICK_SAMPLES; i++) {
    int s = mix[i] << 3;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out_buf[i] = (short)s;
  }
}

static void synth_init(void) {
  short* base = (short*)(g_pak + pak_header(PAKH_WAVE_OFF));
  wave_count = (int)pak_header(PAKH_WAVE_COUNT);
  note_incs = (unsigned*)(g_pak + pak_header(PAKH_NOTE_OFF));
  wave_levels = (short*)mmap(wave_count * LEVELS * 256 * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  for (int w = 0; w < wave_count; w++) {
    for (int l = 0; l < LEVELS; l++) {
      int scale = l * l;                 /* perceptual-ish: quiet levels fall off fast */
      short* dst = wave_levels + ((w * LEVELS + l) << 8);
      for (int i = 0; i < 256; i++) dst[i] = (short)((base[(w << 8) + i] * scale) / 225);
    }
  }
}

/* The synthesizer process: never returns. */
static void synth_main(int fd) {
  if (audio_open() < 0) exit(0);
  synth_init();
  int quiet = 0;
  unsigned char cmds[16];
  while (1) {
    int avail = fd_bytes_available(fd);
    while (avail > 0) {
      int n = read(fd, cmds, avail < 16 ? avail : 16);
      if (n <= 0) break;
      avail -= n;
      for (int i = 0; i < n; i++) {
        int c = cmds[i];
        if (c == CMD_QUIT) {
          audio_close();
          exit(0);
        } else if (c == CMD_ALIVE) {
          quiet = 0;
        } else if (c >= CMD_MUSIC) {
          start_song(c - CMD_MUSIC);
        } else {
          sfx_start(c);
        }
      }
    }
    /* Batch the work: when the device is ahead, nap until about LOW_BYTES
     * remain; otherwise generate enough control ticks to refill to
     * TARGET_BYTES in one go. Every wake costs several kernel calls and a
     * context switch, and the profiler showed one-jiffy naps here taking
     * most of the game core's time. A jiffy is about 4 ms; NAP_BYTES per
     * jiffy of nap is deliberately generous so the queue never runs dry. */
    int queued = audio_buffered();
    if (queued > LOW_BYTES) {
      int naps = (queued - LOW_BYTES) / NAP_BYTES;
      sleep(naps < 1 ? 1 : naps);
      continue;
    }
    int ticks = (TARGET_BYTES - queued) / (TICK_SAMPLES * 2) + 1;
    for (int t = 0; t < ticks; t++) {
      if (++quiet > HEARTBEAT_TIMEOUT) {
        audio_close();
        exit(0);
      }
      sequencer_tick();
      sfx_tick();
      for (int k = 0; k < VOICES; k++) voice_control(&voices[k]);
      mix_tick();
      int done = 0;
      while (done < TICK_SAMPLES * 2) {
        int accepted = audio_write((char*)out_buf + done, TICK_SAMPLES * 2 - done);
        if (accepted <= 0) {
          sleep(1);
          continue;
        }
        done += accepted;
      }
    }
  }
}

/* Fork the synthesizer. Returns 0, or -1 if sound is unavailable (the game
 * plays silently). Call right after pak_open(). */
int audio_start(void) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[1]);
    synth_main(fds[0]);
  }
  close(fds[0]);
  cmd_fd = fds[1];
  audio_ok = 1;
  return 0;
}
