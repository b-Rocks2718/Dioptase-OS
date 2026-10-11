#include "delve.h"
#include "../crt/sys.h"
#include "../crt/unistd.h"

/*
 * Sound: a small synthesizer running in its own process.
 *
 * audio_start() forks a child that owns the PCM device (docs/syscalls.md,
 * "Audio": signed 16-bit mono at 25 kHz from a ~0.33 s ring). Keeping it in
 * a separate process means the music never stutters when a frame takes long,
 * and the scheduler can run it on another core. The game talks to it over a
 * pipe with one-byte commands:
 *
 *   1..63      play sound effect N (SFX_* in delve.h)
 *   64 + song  switch music (MUSIC_*; MUSIC_NONE silences it)
 *   CMD_ALIVE  heartbeat, sent about twice a second
 *   CMD_QUIT   release the device and exit
 *
 * The child also exits if the heartbeat stops for HEARTBEAT_TIMEOUT frames,
 * so it can never outlive a game that crashed or was killed outright. The
 * game's SIGNAL_TERMINATE handler (Ctrl-C in the terminal) sends CMD_QUIT
 * before exiting.
 *
 * Synthesis: eight voices (four music channels, four effect voices), each a
 * phase accumulator stepping through a 64-entry wavetable pre-scaled for 16
 * volume levels, or an LFSR for noise. Envelopes, pitch sweeps, vibrato, and
 * the music sequencer advance at a 200 Hz control rate.
 */

#define CMD_MUSIC 64
#define CMD_ALIVE 0xFE
#define CMD_QUIT 0xFF
#define HEARTBEAT_TIMEOUT 300     /* VGA frames (5 s) without a heartbeat */
#define HEARTBEAT_PERIOD 30

#define CTRL_SAMPLES 125          /* samples per control tick: 200 Hz at 25 kHz */
#define CHUNK_SAMPLES 250         /* two control ticks per audio_write */
#define TARGET_BUFFERED 4500      /* bytes kept queued: 90 ms */
/* Jiffies the synthesizer sleeps between top-ups. Every wake-up costs a few
 * syscalls (about 10,000 instructions each on Dioptase-OS) plus refilling the
 * TLB the context switch emptied, so waking every jiffy cost a third of a
 * core. Six jiffies is a few milliseconds at emulator speeds and well under
 * the 90 ms kept queued even if the emulator runs several times slower. */
#define SYNTH_SLEEP_JIFFIES 6
#define HEARTBEAT_CHECK_LOOPS 16  /* synth loops between heartbeat clock reads */

#define WAVE_SQUARE 0
#define WAVE_PULSE 1
#define WAVE_TRIANGLE 2
#define WAVE_SAW 3
#define WAVE_SINE 4
#define WAVE_NOISE 5
#define WAVE_TABLES 5
#define WAVE_STEPS 64
#define VOLUMES 16
#define AMP_STEP 520              /* peak amplitude per volume level */

#define VOICES 8
#define MUSIC_VOICES 4
#define SFX_FIRST 4
#define SFX_IDS 22

/* ---- Game side ---- */

static int cmd_fd = -1;
static int child = -1;
static bool muted;
static unsigned last_heartbeat;

/* Write one command byte to the synthesizer; a no-op once audio is down.
 * A full pipe blocks briefly, which only happens if the child stalls. */
static void send_byte(int value){
  if (cmd_fd < 0) return;
  unsigned char b = value;
  write(cmd_fd, &b, 1);
}

/* Queue sound effect `id` (SFX_*), unless effects are muted for the demo. */
void sfx(int id){
  if (!muted) send_byte(id);
}

/* Silence sound effects (the attract-mode demo plays with music only). */
void audio_mute_sfx(bool on){
  muted = on;
}

/* Switch the background music to `song` (MUSIC_*). */
void audio_music(int song){
  send_byte(CMD_MUSIC + song);
}

/* Keep the synthesizer alive; call once per frame. */
void audio_heartbeat(void){
  unsigned now = get_vga_frame_counter();
  if (now - last_heartbeat >= HEARTBEAT_PERIOD){
    last_heartbeat = now;
    send_byte(CMD_ALIVE);
  }
}

/* Shut the synthesizer down and reap it, so the device is free on exit. */
void audio_stop(void){
  if (cmd_fd < 0) return;
  send_byte(CMD_QUIT);
  close(cmd_fd);
  cmd_fd = -1;
  if (child >= 0){
    wait_child(child);
    child = -1;
  }
}

/* SIGNAL_TERMINATE handler for the game process (Ctrl-C in the terminal).
 * Runs on the signal stack; stops the synthesizer and exits. */
static void on_terminate(int signal){
  (void)signal;
  send_byte(CMD_QUIT);
  exit(0);
}

/* ---- Synthesizer process ---- */

struct Voice {
  int on;
  unsigned phase;
  unsigned inc;          /* phase step per sample at the base pitch */
  unsigned out_inc;      /* inc with vibrato applied */
  int wave;
  int vol;               /* 16.16 volume level, 0..15 */
  int vol_step;          /* added each control tick */
  int vol_floor;         /* decay stops here (sustain) */
  unsigned lfsr;
  int noise_out;
  int seg;               /* effect segment being played, -1 for music notes */
  int seg_ticks;         /* control ticks left in the segment */
  int inc_step;          /* pitch sweep per control tick */
  int vib_depth;         /* vibrato depth as a 16.16 fraction of pitch */
  int vib_phase;
};

static struct Voice voices[VOICES];
static short wavetab[5120];       /* WAVE_TABLES * VOLUMES * WAVE_STEPS */
static int mix_acc[CHUNK_SAMPLES];
static short out_buf[CHUNK_SAMPLES];

/* Music instruments: lead, arpeggio, bass, drums. */
struct Instrument {
  int wave;
  int peak;      /* starting volume level */
  int decay;     /* 16.16 levels lost per control tick */
  int floor;     /* sustain level */
  int vib;       /* vibrato depth (16.16 fraction of pitch) */
};

static struct Instrument instruments[MUSIC_VOICES] = {
  { WAVE_PULSE,    8, 0x1400, 5, 0x0300 },
  { WAVE_SQUARE,   4, 0x2000, 2, 0 },
  { WAVE_TRIANGLE, 13, 0x0C00, 9, 0 },
  { WAVE_NOISE,    7, 0x9000, 0, 0 },
};

/* The title theme swaps in soft bell-like voices. */
static struct Instrument title_lead = { WAVE_SINE, 12, 0x0C00, 3, 0x0200 };
static struct Instrument title_arp = { WAVE_TRIANGLE, 6, 0x1400, 2, 0 };

/* Sound effect segments: an effect plays its segments in order. */
struct SfxSeg {
  int wave;
  int hz0;
  int hz1;
  int ms;
  int vol0;
  int vol1;
  int last;      /* 1 on the final segment of an effect */
};

static struct SfxSeg sfx_segs[39] = {
  /* 0 SPARK */          { WAVE_SQUARE,  1400, 2600,  90, 10, 0, 1 },
  /* 1 FIRE */           { WAVE_NOISE,   2000,  500, 120, 12, 9, 0 },
  /* 2 */                { WAVE_SAW,      240,   90, 260, 10, 0, 1 },
  /* 3 EXPLODE */        { WAVE_NOISE,   1800,  200, 120, 15, 12, 0 },
  /* 4 */                { WAVE_NOISE,    300,   60, 450, 12, 0, 1 },
  /* 5 FIZZLE */         { WAVE_NOISE,   6000, 3000, 110, 7, 0, 1 },
  /* 6 MONSTER_HURT */   { WAVE_SQUARE,   520,  200, 130, 10, 2, 1 },
  /* 7 MONSTER_DIE */    { WAVE_SQUARE,   700,  300, 120, 11, 8, 0 },
  /* 8 */                { WAVE_SAW,      300,   50, 380, 10, 0, 1 },
  /* 9 PLAYER_HURT */    { WAVE_SAW,      260,  110, 220, 14, 0, 1 },
  /* 10 CRYSTAL */       { WAVE_SINE,    1568, 1568,  70, 12, 8, 0 },
  /* 11 */               { WAVE_SINE,    2093, 2093, 260, 12, 0, 1 },
  /* 12 POTION */        { WAVE_TRIANGLE, 300,  700,  90, 11, 9, 0 },
  /* 13 */               { WAVE_TRIANGLE, 400,  900,  90, 11, 9, 0 },
  /* 14 */               { WAVE_TRIANGLE, 500, 1200, 140, 11, 0, 1 },
  /* 15 KEY */           { WAVE_SQUARE,  1047, 1047,  70, 9, 7, 0 },
  /* 16 */               { WAVE_SQUARE,  1319, 1319,  70, 9, 7, 0 },
  /* 17 */               { WAVE_SQUARE,  1568, 1568,  70, 9, 7, 0 },
  /* 18 */               { WAVE_SQUARE,  2093, 2093, 300, 9, 0, 1 },
  /* 19 DOOR */          { WAVE_NOISE,    500,  250, 380, 8, 0, 1 },
  /* 20 STEP */          { WAVE_NOISE,    260,  140,  45, 4, 0, 1 },
  /* 21 GROWL */         { WAVE_SAW,       95,   70, 450, 12, 0, 1 },
  /* 22 ENEMY_SHOOT */   { WAVE_PULSE,    900,  300, 180, 10, 0, 1 },
  /* 23 LOCKED */        { WAVE_SQUARE,   120,  120,  90, 10, 10, 0 },
  /* 24 */               { WAVE_SQUARE,     1,    1,  50, 0, 0, 0 },
  /* 25 */               { WAVE_SQUARE,   110,  110, 140, 10, 0, 1 },
  /* 26 PORTAL */        { WAVE_TRIANGLE, 200, 1600, 900, 12, 4, 0 },
  /* 27 */               { WAVE_SINE,    1600, 2400, 300, 8, 0, 1 },
  /* 28 NOMANA */        { WAVE_PULSE,    180,  150,  90, 8, 0, 1 },
  /* 29 DEATH */         { WAVE_SQUARE,   440,  415, 260, 12, 10, 0 },
  /* 30 */               { WAVE_SQUARE,   370,  349, 260, 11, 9, 0 },
  /* 31 */               { WAVE_SQUARE,   294,  262, 260, 10, 8, 0 },
  /* 32 */               { WAVE_SAW,      220,   55, 900, 12, 0, 1 },
  /* 33 BUMP */          { WAVE_NOISE,    160,  100,  60, 6, 0, 1 },
  /* 34 DRIP */          { WAVE_SINE,    1500,  520,  60, 6, 0, 0 },
  /* 35 */               { WAVE_SINE,       1,    1, 140, 0, 0, 0 },
  /* 36 */               { WAVE_SINE,    1400,  480,  50, 2, 0, 1 },
  /* 37 CRUMBLE */       { WAVE_NOISE,    900,  120, 200, 15, 10, 0 },
  /* 38 */               { WAVE_NOISE,    200,   40, 700, 11, 0, 1 },
};

/* First segment of each SFX_* id (index = id, -1 = unused). */
static int sfx_start[SFX_IDS] = {
  -1, 0, 1, 3, 5, 6, 7, 9, 10, 12, 15, 19, 20, 21, 22, 23, 26, 28, 29, 33, 34, 37
};

/* Sequencer state. */
static int song = MUSIC_NONE;
static int song_row;
static int song_rows;
static int song_loop;
static int row_samples;
static int row_left;
static unsigned char* song_rows_data;
static struct Instrument* chan_inst[MUSIC_VOICES];

/* Fill wavetab: every wave shape pre-scaled for each of the 16 volume
 * levels, so mixing needs no multiply per sample. */
static void build_wavetables(void){
  for (int w = 0; w < WAVE_TABLES; ++w){
    for (int v = 0; v < VOLUMES; ++v){
      int amp = v * AMP_STEP;
      for (int i = 0; i < WAVE_STEPS; ++i){
        int s;   /* -1024 .. 1024 */
        switch (w){
          case WAVE_SQUARE: s = i < 32 ? 1024 : -1024; break;
          case WAVE_PULSE: s = i < 16 ? 1024 : -1024; break;
          case WAVE_TRIANGLE: s = i < 32 ? -1024 + i * 64 : 1024 - (i - 32) * 64; break;
          case WAVE_SAW: s = -1024 + i * 32; break;
          default: s = sin_table[i * 64] >> 6; break;
        }
        wavetab[(w * VOLUMES + v) * WAVE_STEPS + i] = (s * amp) >> 10;
      }
    }
  }
}

/* Phase increment for a frequency in Hz (2^32 / 25000 = 171798.69). */
static unsigned hz_inc(int hz){
  return (unsigned)hz * 171799u;
}

/* Start a music note on voice `v` with instrument `inst` (MIDI note number). */
static void voice_note(struct Voice* v, struct Instrument* inst, int note){
  v->on = 1;
  v->wave = inst->wave;
  v->inc = note_incs[note & 127];
  v->out_inc = v->inc;
  v->vol = inst->peak << 16;
  v->vol_step = -inst->decay;
  v->vol_floor = inst->floor << 16;
  v->vib_depth = inst->vib;
  v->vib_phase = 0;
  v->seg = -1;
  if (inst->wave == WAVE_NOISE){
    /* drums: low notes ring longer, high notes are short ticks */
    v->vol_step = note < 70 ? -0x9000 : (note < 110 ? -0x6000 : -0x18000);
  }
}

/* Let a music note fade out (note-off). */
static void voice_release(struct Voice* v){
  v->vol_floor = 0;
  v->vol_step = -0x8000;
}

/* Load effect segment `index` into a voice. */
static void voice_segment(struct Voice* v, int index){
  struct SfxSeg* s = &sfx_segs[index];
  int ticks = s->ms / 5;
  if (ticks < 1) ticks = 1;
  v->on = 1;
  v->seg = index;
  v->seg_ticks = ticks;
  v->wave = s->wave;
  v->inc = hz_inc(s->hz0);
  v->out_inc = v->inc;
  v->inc_step = ((int)hz_inc(s->hz1) - (int)hz_inc(s->hz0)) / ticks;
  v->vol = s->vol0 << 16;
  v->vol_step = ((s->vol1 - s->vol0) << 16) / ticks;
  v->vol_floor = 0;
  v->vib_depth = 0;
}

static int next_sfx_voice;

/* Begin effect `id` on a free effect voice, stealing the oldest if all
 * four are busy. */
static void start_sfx(int id){
  if (id <= 0 || id >= SFX_IDS) return;
  int seg = sfx_start[id];
  if (seg < 0) return;
  /* prefer an idle effect voice, else steal round-robin */
  int pick = -1;
  for (int i = SFX_FIRST; i < VOICES; ++i){
    if (!voices[i].on){
      pick = i;
      break;
    }
  }
  if (pick < 0){
    pick = SFX_FIRST + next_sfx_voice;
    next_sfx_voice = (next_sfx_voice + 1) & 3;
  }
  voice_segment(&voices[pick], seg);
}

/* Reset the sequencer to the start of song `id` (or silence for MUSIC_NONE). */
static void start_song(int id){
  song = id;
  for (int i = 0; i < MUSIC_VOICES; ++i){
    voices[i].on = 0;
    chan_inst[i] = &instruments[i];
  }
  if (id == MUSIC_TITLE){
    chan_inst[0] = &title_lead;
    chan_inst[1] = &title_arp;
  }
  if (id <= MUSIC_NONE) return;
  int* dir = song_table + (id - 1) * 4;
  row_samples = dir[0];
  song_rows = dir[1];
  song_loop = dir[2];
  song_rows_data = song_data + dir[3];
  song_row = 0;
  row_left = 0;
}

/* Advance the sequencer by one row (an eighth note). */
static void song_step(void){
  if (song <= MUSIC_NONE) return;
  if (song_row >= song_rows){
    if (!song_loop){
      song = MUSIC_NONE;
      for (int i = 0; i < MUSIC_VOICES; ++i) voice_release(&voices[i]);
      return;
    }
    song_row = 0;
  }
  unsigned char* row = song_rows_data + song_row * 4;
  for (int c = 0; c < MUSIC_VOICES; ++c){
    int n = row[c];
    if (n == 1) voice_release(&voices[c]);
    else if (n > 1) voice_note(&voices[c], chan_inst[c], n);
  }
  song_row += 1;
}

/* Control-rate update: envelopes, sweeps, vibrato, effect programs. */
static void control_tick(void){
  for (int i = 0; i < VOICES; ++i){
    struct Voice* v = &voices[i];
    if (!v->on) continue;
    v->vol += v->vol_step;
    if (v->vol_step < 0 && v->vol < v->vol_floor) v->vol = v->vol_floor;
    if (v->vol > (15 << 16)) v->vol = 15 << 16;
    if (v->seg >= 0){
      v->inc += v->inc_step;
      v->out_inc = v->inc;
      v->seg_ticks -= 1;
      if (v->seg_ticks <= 0){
        if (sfx_segs[v->seg].last){
          v->on = 0;
          v->seg = -1;
        } else {
          voice_segment(v, v->seg + 1);
        }
      }
      continue;
    }
    if (v->vol <= 0){
      v->vol = 0;
      v->on = 0;
      continue;
    }
    if (v->vib_depth){
      v->vib_phase += 40;
      /* inc * (1 + depth * sin): the 16.16 product of inc>>16 keeps 32 bits */
      int wobble = fixmul(isin(v->vib_phase), v->vib_depth);
      v->out_inc = v->inc + (unsigned)fixmul((int)(v->inc >> 4), wobble) * 16;
    }
  }
}

/* Mix one chunk into out_buf. */
static void mix(void){
  for (int i = 0; i < CHUNK_SAMPLES; ++i) mix_acc[i] = 0;
  for (int k = 0; k < VOICES; ++k){
    struct Voice* v = &voices[k];
    if (!v->on) continue;
    int level = v->vol >> 16;
    if (level <= 0) continue;
    unsigned phase = v->phase;
    unsigned inc = v->out_inc;
    if (v->wave == WAVE_NOISE){
      /* the LFSR steps each time the phase wraps, so pitch sets the color */
      unsigned lfsr = v->lfsr | 1;
      int amp = level * AMP_STEP;
      int out = v->noise_out;
      for (int i = 0; i < CHUNK_SAMPLES; ++i){
        unsigned next = phase + inc;
        if (next < phase){
          lfsr = (lfsr >> 1) ^ (-(lfsr & 1) & 0xB400u);
          out = (lfsr & 1) ? amp : -amp;
        }
        phase = next;
        mix_acc[i] += out;
      }
      v->lfsr = lfsr;
      v->noise_out = out;
    } else {
      short* table = wavetab + (v->wave * VOLUMES + level) * WAVE_STEPS;
      mix_add(mix_acc, CHUNK_SAMPLES, table, phase, inc);
      phase += inc * CHUNK_SAMPLES;
    }
    v->phase = phase;
  }
  for (int i = 0; i < CHUNK_SAMPLES; ++i){
    int s = mix_acc[i];
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out_buf[i] = s;
  }
}

/* Synthesizer main loop (child process). */
static void synth_main(int fd){
  if (audio_open() < 0){
    /* no audio device (or busy): keep draining commands quietly */
    while (true){
      unsigned char c;
      if (read(fd, &c, 1) != 1 || c == CMD_QUIT) exit(0);
    }
  }
  request_priority(DIOPTASE_PRIORITY_HIGH);
  pin_core(CORE_AUDIO);
  build_wavetables();
  unsigned last_alive = get_vga_frame_counter();
  int pending = 0;            /* bytes of out_buf not yet accepted */
  unsigned char cmds[32];
  int loops = 0;
  while (true){
    loops += 1;
    int avail = fd_bytes_available(fd);
    if (avail > 0){
      int n = read(fd, cmds, avail > 32 ? 32 : avail);
      for (int i = 0; i < n; ++i){
        int c = cmds[i];
        if (c == CMD_QUIT){
          audio_close();
          exit(0);
        }
        if (c == CMD_ALIVE) continue;
        if (c >= CMD_MUSIC) start_song(c - CMD_MUSIC);
        else start_sfx(c);
      }
      loops = 0;
      last_alive = get_vga_frame_counter();
    } else if ((loops & (HEARTBEAT_CHECK_LOOPS - 1)) == 0 &&
               get_vga_frame_counter() - last_alive > HEARTBEAT_TIMEOUT){
      audio_close();
      exit(0);
    }

    int buffered = audio_buffered();
    while (buffered >= 0 && buffered < TARGET_BUFFERED){
      if (pending == 0){
        for (int s = 0; s < CHUNK_SAMPLES; s += CTRL_SAMPLES){
          if (song > MUSIC_NONE){
            row_left -= CTRL_SAMPLES;
            if (row_left <= 0){
              row_left += row_samples;
              song_step();
            }
          }
          control_tick();
        }
        mix();
        pending = CHUNK_SAMPLES * 2;
      }
      int done = CHUNK_SAMPLES * 2 - pending;
      int accepted = audio_write((char*)out_buf + done, pending);
      if (accepted <= 0) break;
      pending -= accepted;
      buffered += accepted;
    }
    sleep(SYNTH_SLEEP_JIFFIES);
  }
}

/* Fork the synthesizer process and connect its command pipe. Must run
 * before anything else forks or registers handlers: the child inherits the
 * parent's handler table, so SIGNAL_TERMINATE is registered afterwards and
 * only in the game process. Audio failures leave the game silent but
 * playable. */
void audio_start(void){
  int fds[2];
  if (pipe(fds) < 0) return;
  int pid = fork();
  if (pid == 0){
    close(fds[1]);
    synth_main(fds[0]);
    exit(0);
  }
  close(fds[0]);
  if (pid < 0){
    close(fds[1]);
    return;
  }
  child = pid;
  cmd_fd = fds[1];
  last_heartbeat = get_vga_frame_counter();
  register_handler(SIGNAL_TERMINATE, on_terminate);
}
