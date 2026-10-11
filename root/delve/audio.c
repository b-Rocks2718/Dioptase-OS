#include "delve.h"
#include "music_data.h"
#include "../crt/sys.h"
#include "../crt/unistd.h"

/*
 * Sound: a synthesizer running in its own process.
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
 * Music is a sample-playback tracker. A song (tools/music.py, tables in
 * assets.s and music_data.h, samples in MUSIC_FILE, read into memory when
 * the synthesizer starts) is an order list of patterns; a pattern is a
 * byte stream of note events on MUSIC_CHANNELS channels on a grid of rows.
 * A note plays one sampled instrument (8-bit mu-law) at a pitch set by its
 * 16.16 step through the sample, looping the sustained part of instruments
 * that have one, under an attack/decay/sustain/release envelope. When a new
 * note cuts a sounding one, the old note moves to its channel's tail voice
 * and fades out within a few milliseconds instead of clicking off.
 *
 * Sound effects come from a small wavetable synthesizer: four voices, each a
 * phase accumulator stepping through a 64-entry wave pre-scaled for 16
 * volume levels (or an LFSR for noise), with pitch and volume sweeps.
 *
 * Envelopes, sweeps, and the sequencer advance at a 200 Hz control rate.
 * Every chunk the voices are summed, passed through a small reverb (two
 * damped feedback combs into an allpass, reverb_mix in gfx.s), clipped, and
 * written to the device. Mixing needs no multiplies: music voices read a
 * table of each mu-law code pre-scaled for 64 levels, and the effect waves
 * are pre-scaled for 16.
 */

#define CMD_MUSIC 64
#define CMD_ALIVE 0xFE
#define CMD_QUIT 0xFF
#define HEARTBEAT_TIMEOUT 300     /* VGA frames (5 s) without a heartbeat */
#define HEARTBEAT_PERIOD 30

#define CTRL_SAMPLES 125          /* samples per control tick: 200 Hz at 25 kHz */
#define CHUNK_SAMPLES 250         /* two control ticks per audio_write */
#define CHUNK_BYTES 500

/* How much audio to keep queued in the device ring. Less means quicker
 * sound effects; more rides out longer stalls (a busy host can hold the
 * emulator back for tens of milliseconds). The target starts low and grows
 * by BUFFER_GROW each time the ring is found nearly empty, then creeps back
 * down after BUFFER_CALM_FRAMES without another scare. In bytes: 2 per
 * sample, 50 per millisecond. */
#define BUFFER_MIN 5000           /* 100 ms */
#define BUFFER_MAX 13000          /* 260 ms, inside the ~0.33 s ring */
#define BUFFER_GROW 1500
#define BUFFER_SHRINK 250
#define BUFFER_STARVED 1000       /* under 20 ms left: we were nearly late */
#define BUFFER_CALM_FRAMES 600    /* 10 s */

/* Jiffies the synthesizer sleeps between top-ups. Every wake-up costs a few
 * syscalls (about 10,000 instructions each on Dioptase-OS) plus refilling the
 * TLB the context switch emptied, so waking every jiffy cost a third of a
 * core. Six jiffies is a few milliseconds at emulator speeds, well under the
 * 100 ms or more kept queued. */
#define SYNTH_SLEEP_JIFFIES 6
#define HEARTBEAT_CHECK_LOOPS 16  /* synth loops between heartbeat clock reads */

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

/* ---- Synthesizer process: shared mix buffers ---- */

static int mix_acc[CHUNK_SAMPLES];
static short out_buf[CHUNK_SAMPLES];

/* ---- Music: sample tracker ---- */

#define ENV_FULL 16384
#define PITCH_CENTER 64           /* pitch_steps index that plays a sample as recorded */
#define LEVELS 64
#define VOL_SHIFT 8               /* level-l table entry = linear * l >> VOL_SHIFT */
#define MASTER_FULL 64
#define TAIL_RELEASE 4096         /* a cut note fades over four control ticks */

/* Pattern stream codes (tools/music.py): a note is EV_NOTE + channel, then
 * note, instrument, and velocity bytes; EV_OFF + channel releases; EV_VOL +
 * channel, then a velocity, sets the channel volume; EV_WAIT + n - 1 waits
 * n rows; EV_END ends the pattern. */
#define EV_NOTE 0x00
#define EV_OFF 0x08
#define EV_VOL 0x10
#define EV_WAIT 0x80
#define EV_END 0xFF

#define STAGE_ATTACK 0
#define STAGE_DECAY 1
#define STAGE_SUSTAIN 2
#define STAGE_RELEASE 3

/* One sounding note. The layout of the first five fields is what mix_voice
 * hands mix_sample (gfx.s). */
struct MusicVoice {
  unsigned char* data;   /* the instrument's mu-law samples; NULL when silent */
  unsigned pos;          /* 16.16 position in the sample */
  unsigned step;         /* 16.16 advance per output sample (the pitch) */
  unsigned end;          /* sample length << 16 */
  unsigned loop_len;     /* looped part's length << 16; 0 for a one-shot */
  int env;               /* envelope, 0 .. ENV_FULL */
  int stage;             /* STAGE_* */
  int attack;            /* envelope units per control tick */
  int decay;
  int sustain;
  int release;
  int vel;               /* note velocity 0..63 */
  int level;             /* mixing level 0..63, set every control tick */
};

/* Channel c plays music_voices[c]; tail_voices[c] fades out the note the
 * last new note on c cut. */
static struct MusicVoice music_voices[MUSIC_CHANNELS];
static struct MusicVoice tail_voices[MUSIC_CHANNELS];
static int chan_vol[MUSIC_CHANNELS];      /* 0..63, set by EV_VOL events */

/* vol_table[(level << 8) + code]: mu-law code `code` at mixing level
 * `level`, LEVELS * 256 halfwords. Like the reverb's delay lines, it lives
 * in memory the synthesizer maps at startup (synth_alloc), not in the
 * program image: the game and render processes never need it, and the image
 * must stay small enough for PC-relative addressing to reach its data. */
#define VOL_TABLE_BYTES 32768
static short* vol_table;
/* scale6[(a << 6) + b] = a * b >> 6 for a in 0..64, b in 0..63 (65 * 64). */
static unsigned char scale6[4160];

/* The instruments' mu-law samples: MUSIC_FILE mapped read-only; NULL if
 * the file is missing (the game then has effects but no music). Pages come
 * in from the disk on first touch, which takes about 15 ms each, so the
 * synthesizer touches a few more every loop (warm_music_pages) until all
 * are resident, keeping a deeper buffer meanwhile; a note that needs a page
 * sooner simply faults it in. Music can start the moment the file is
 * mapped instead of after a second or two of loading. */
static unsigned char* music_samples;
static int music_warm_offset;          /* first byte not yet touched */
static unsigned music_touch_sum;       /* keeps the touching loads */
#define WARM_PAGES_PER_LOOP 2

/* Sequencer state. */
static int song = MUSIC_NONE;          /* playing (or fading out) */
static int queued_song = MUSIC_NONE;   /* starts when the fade-out ends */
static bool fading;
static int master = MASTER_FULL;
static int row_samples;
static int row_left;                   /* samples until the next row */
static int rows_left;                  /* rows until the next event */
static unsigned char* events;          /* read position in the pattern */
static int order_first;
static int order_count;
static int order_index;
static int order_loop;                 /* -1: the song stops at its end */

/* Map MUSIC_FILE (MUSIC_SAMPLE_BYTES long) read-only. Mapping beats
 * read(): the kernel moves at most 1 KB per read() call, through a
 * temporary mapping of its own, which made loading take seconds. Returns
 * false, leaving music_samples NULL, if the file is missing or cannot be
 * mapped. */
static bool load_music_samples(void){
  int fd = open(MUSIC_FILE);
  if (fd < 0) return false;
  unsigned char* mem = (unsigned char*)mmap(MUSIC_SAMPLE_BYTES, fd, 0, MMAP_READ);
  close(fd);
  if ((int)mem == -1) return false;
  music_samples = mem;
  music_warm_offset = 0;
  return true;
}

/* Fault in the next few pages of the samples. Returns true while some are
 * still untouched. */
static bool warm_music_pages(void){
  if (music_samples == NULL || music_warm_offset >= MUSIC_SAMPLE_BYTES) return false;
  volatile unsigned char* bytes = music_samples;
  for (int i = 0; i < WARM_PAGES_PER_LOOP && music_warm_offset < MUSIC_SAMPLE_BYTES; ++i){
    music_touch_sum += bytes[music_warm_offset];
    music_warm_offset += 4096;
  }
  return true;
}

/* Fill the multiply-free tables (one-time, ~16k additions). */
static void build_music_tables(void){
  for (int code = 0; code < 256; ++code){
    int linear = mulaw_table[code];
    int sum = 0;
    for (int level = 0; level < LEVELS; ++level){
      vol_table[(level << 8) + code] = sum >> VOL_SHIFT;
      sum += linear;
    }
  }
  for (int a = 0; a <= 64; ++a){
    int sum = 0;
    for (int b = 0; b < 64; ++b){
      scale6[(a << 6) + b] = sum >> 6;
      sum += a;
    }
  }
  for (int c = 0; c < MUSIC_CHANNELS; ++c) chan_vol[c] = 63;
}

/* Start note `note` (MIDI) of instrument `inst` on channel `ch`; a note
 * already sounding there moves to the channel's tail voice to fade out. */
static void music_note(int ch, int note, int inst, int vel){
  struct MusicVoice* v = &music_voices[ch];
  if (v->data != NULL){
    tail_voices[ch] = *v;
    tail_voices[ch].stage = STAGE_RELEASE;
    tail_voices[ch].release = TAIL_RELEASE;
  }
  int* w = music_insts + (inst << 3);
  int idx = note - w[3] + PITCH_CENTER;
  if (idx < 0) idx = 0;
  if (idx > 127) idx = 127;
  v->data = music_samples + w[0];
  v->pos = 0;
  v->step = pitch_steps[idx];
  v->end = (unsigned)w[1] << 16;
  v->loop_len = w[2] < w[1] ? (unsigned)(w[1] - w[2]) << 16 : 0;
  v->attack = w[4];
  v->decay = w[5];
  v->sustain = w[6];
  v->release = w[7];
  v->env = 0;
  v->stage = STAGE_ATTACK;
  v->vel = vel;
  v->level = 0;
}

/* Silence every music voice at once (between songs). */
static void music_silence(void){
  for (int c = 0; c < MUSIC_CHANNELS; ++c){
    music_voices[c].data = NULL;
    tail_voices[c].data = NULL;
    chan_vol[c] = 63;
  }
}

/* Point the sequencer at order entry `order_index` of the current song. */
static void music_load_pattern(void){
  int pattern = music_orders[order_first + order_index];
  events = music_events + music_pattern_offsets[pattern];
}

/* Begin song `id` (MUSIC_*) from its first pattern; MUSIC_NONE stops. */
static void music_start(int id){
  music_silence();
  song = id;
  master = MASTER_FULL;
  if (id <= MUSIC_NONE || id > MUSIC_SONGS || music_samples == NULL){
    song = MUSIC_NONE;
    return;
  }
  int* s = music_songs + ((id - 1) << 2);
  row_samples = s[0];
  order_first = s[1];
  order_count = s[2];
  order_loop = s[3];
  order_index = 0;
  row_left = 0;
  rows_left = 0;
  music_load_pattern();
}

/* Switch to song `id`: a song already playing fades out first (FADE: one
 * master step per control tick, 64 ticks), and asking for the song that is
 * already playing changes nothing. */
static void music_request(int id){
  if (id == song && !fading) return;
  if (song == MUSIC_NONE){
    music_start(id);
    return;
  }
  queued_song = id;
  fading = true;
}

/* Advance the sequencer by one control tick, starting the events of every
 * row that comes due. */
static void music_sequence(void){
  if (song == MUSIC_NONE) return;
  row_left -= CTRL_SAMPLES;
  while (row_left <= 0){
    row_left += row_samples;
    if (rows_left > 0) rows_left -= 1;
    while (rows_left == 0){
      int code = *events++;
      if (code == EV_END){
        order_index += 1;
        if (order_index >= order_count){
          if (order_loop < 0){
            /* a song that plays once: let its last notes ring out */
            for (int c = 0; c < MUSIC_CHANNELS; ++c) music_voices[c].stage = STAGE_RELEASE;
            song = MUSIC_NONE;
            return;
          }
          order_index = order_loop;
        }
        music_load_pattern();
      } else if (code >= EV_WAIT){
        rows_left = code - EV_WAIT + 1;
      } else if (code >= EV_VOL){
        chan_vol[code - EV_VOL] = *events++;
      } else if (code >= EV_OFF){
        music_voices[code - EV_OFF].stage = STAGE_RELEASE;
      } else {
        int note = events[0];
        int inst = events[1];
        int vel = events[2];
        events += 3;
        music_note(code - EV_NOTE, note, inst, vel);
      }
    }
  }
}

/* One control tick of a voice's envelope; sets its mixing level from the
 * envelope, note velocity, channel volume, and the master fade. */
static void voice_envelope(struct MusicVoice* v, int cvol){
  if (v->data == NULL) return;
  switch (v->stage){
    case STAGE_ATTACK:
      v->env += v->attack;
      if (v->env >= ENV_FULL){
        v->env = ENV_FULL;
        v->stage = STAGE_DECAY;
      }
      break;
    case STAGE_DECAY:
      v->env -= v->decay;
      if (v->env <= v->sustain){
        v->env = v->sustain;
        v->stage = STAGE_SUSTAIN;
      }
      break;
    case STAGE_RELEASE:
      v->env -= v->release;
      if (v->env <= 0){
        v->data = NULL;
        return;
      }
      break;
    default:
      break;
  }
  int vv = scale6[(cvol << 6) + v->vel];
  int level = scale6[((v->env >> 8) << 6) + vv];
  v->level = scale6[(master << 6) + level];
}

/* Control tick for all music voices and the master fade. */
static void music_control(void){
  if (fading){
    master -= 1;
    if (master <= 0){
      fading = false;
      music_start(queued_song);
    }
  }
  music_sequence();
  for (int c = 0; c < MUSIC_CHANNELS; ++c){
    voice_envelope(&music_voices[c], chan_vol[c]);
    voice_envelope(&tail_voices[c], chan_vol[c]);
  }
}

/* Add one music voice to the chunk; a one-shot that runs out goes silent. */
static void mix_voice(struct MusicVoice* v){
  if (v->data == NULL || v->level <= 0) return;
  unsigned pos = mix_sample(mix_acc, CHUNK_SAMPLES, v->data, v->pos, v->step,
                            v->end, v->loop_len, vol_table + (v->level << 8));
  if (pos >= v->end) v->data = NULL;
  else v->pos = pos;
}

/* ---- Sound effects: wavetable synthesizer ---- */

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
#define SFX_VOICES 4
#define SFX_IDS 22

struct Voice {
  int on;
  unsigned phase;
  unsigned inc;          /* phase step per sample */
  int wave;
  int vol;               /* 16.16 volume level, 0..15 */
  int vol_step;          /* added each control tick */
  unsigned lfsr;
  int noise_out;
  int seg;               /* effect segment being played */
  int seg_ticks;         /* control ticks left in the segment */
  int inc_step;          /* pitch sweep per control tick */
};

static struct Voice sfx_voices[SFX_VOICES];
static short wavetab[5120];       /* WAVE_TABLES * VOLUMES * WAVE_STEPS */

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
  v->inc_step = ((int)hz_inc(s->hz1) - (int)hz_inc(s->hz0)) / ticks;
  v->vol = s->vol0 << 16;
  v->vol_step = ((s->vol1 - s->vol0) << 16) / ticks;
}

static int next_sfx_voice;

/* Begin effect `id` on a free effect voice, stealing the oldest if all
 * four are busy. */
static void start_sfx(int id){
  if (id <= 0 || id >= SFX_IDS) return;
  int seg = sfx_start[id];
  if (seg < 0) return;
  int pick = -1;
  for (int i = 0; i < SFX_VOICES; ++i){
    if (!sfx_voices[i].on){
      pick = i;
      break;
    }
  }
  if (pick < 0){
    pick = next_sfx_voice;
    next_sfx_voice = (next_sfx_voice + 1) & (SFX_VOICES - 1);
  }
  voice_segment(&sfx_voices[pick], seg);
}

/* Control tick for the effect voices: volume and pitch sweeps, and moving
 * on through each effect's segments. */
static void sfx_control(void){
  for (int i = 0; i < SFX_VOICES; ++i){
    struct Voice* v = &sfx_voices[i];
    if (!v->on) continue;
    v->vol += v->vol_step;
    if (v->vol < 0) v->vol = 0;
    if (v->vol > (15 << 16)) v->vol = 15 << 16;
    v->inc += v->inc_step;
    v->seg_ticks -= 1;
    if (v->seg_ticks <= 0){
      if (sfx_segs[v->seg].last) v->on = 0;
      else voice_segment(v, v->seg + 1);
    }
  }
}

/* Add the effect voices to the chunk. */
static void mix_sfx(void){
  for (int k = 0; k < SFX_VOICES; ++k){
    struct Voice* v = &sfx_voices[k];
    if (!v->on) continue;
    int level = v->vol >> 16;
    if (level <= 0) continue;
    unsigned phase = v->phase;
    unsigned inc = v->inc;
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
}

/* ---- Reverb and output ---- */

/* Reverb state, laid out for reverb_mix (gfx.s): each delay line's base,
 * end, and read/write pointer, then the combs' damping filter states. The
 * lines hold the mix's recent past: 3511, 4813, and 577 samples, coprime so
 * their echoes do not line up. */
#define COMB1_LEN 3511
#define COMB2_LEN 4813
#define ALLPASS_LEN 577
#define REVERB_BYTES 35604        /* (3511 + 4813 + 577) words */
static struct Reverb reverb;

/* Map the synthesizer's large tables (the volume table and the reverb's
 * delay lines) as one zeroed anonymous region and point the reverb at its
 * lines. Returns false if there is no memory for them. */
static bool synth_alloc(void){
  char* mem = (char*)mmap(VOL_TABLE_BYTES + REVERB_BYTES, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  if ((int)mem == -1) return false;
  vol_table = (short*)mem;
  int* lines = (int*)(mem + VOL_TABLE_BYTES);
  reverb.comb1 = lines;
  reverb.comb1_end = lines + COMB1_LEN;
  reverb.comb1_at = lines;
  reverb.comb2 = reverb.comb1_end;
  reverb.comb2_end = reverb.comb2 + COMB2_LEN;
  reverb.comb2_at = reverb.comb2;
  reverb.allpass = reverb.comb2_end;
  reverb.allpass_end = reverb.allpass + ALLPASS_LEN;
  reverb.allpass_at = reverb.allpass;
  reverb.lp1 = 0;
  reverb.lp2 = 0;
  return true;
}

/* Produce one chunk: control ticks, then music, effects, and reverb. */
static void render_chunk(void){
  for (int s = 0; s < CHUNK_SAMPLES; s += CTRL_SAMPLES){
    music_control();
    sfx_control();
  }
  for (int i = 0; i < CHUNK_SAMPLES; ++i) mix_acc[i] = 0;
  for (int c = 0; c < MUSIC_CHANNELS; ++c){
    mix_voice(&music_voices[c]);
    mix_voice(&tail_voices[c]);
  }
  mix_sfx();
  reverb_mix(mix_acc, out_buf, CHUNK_SAMPLES, &reverb);
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
  if (!synth_alloc()){
    /* no memory for the mixer: keep draining commands quietly */
    audio_close();
    while (true){
      unsigned char c;
      if (read(fd, &c, 1) != 1 || c == CMD_QUIT) exit(0);
    }
  }
  build_wavetables();
  build_music_tables();
  load_music_samples();
  unsigned last_alive = get_vga_frame_counter();
  unsigned calm_since = last_alive;
  int target = BUFFER_MIN;
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
        if (c >= CMD_MUSIC) music_request(c - CMD_MUSIC);
        else start_sfx(c);
      }
      loops = 0;
      last_alive = get_vga_frame_counter();
    } else if ((loops & (HEARTBEAT_CHECK_LOOPS - 1)) == 0 &&
               get_vga_frame_counter() - last_alive > HEARTBEAT_TIMEOUT){
      audio_close();
      exit(0);
    }

    /* while the samples are still coming in from the disk, keep the ring
     * as full as it goes: a page fault can stall this loop */
    bool warming = warm_music_pages();
    int buffered = audio_buffered();
    /* Nearly drained: we woke too late for comfort, so keep more queued
     * from now on. After a long calm, give some of it back. */
    unsigned now = get_vga_frame_counter();
    if (buffered >= 0 && buffered < BUFFER_STARVED && !warming){
      target += BUFFER_GROW;
      if (target > BUFFER_MAX) target = BUFFER_MAX;
      calm_since = now;
    } else if (now - calm_since > BUFFER_CALM_FRAMES){
      target -= BUFFER_SHRINK;
      if (target < BUFFER_MIN) target = BUFFER_MIN;
      calm_since = now;
    }
    int fill_to = warming ? BUFFER_MAX : target;
    while (buffered >= 0 && buffered < fill_to){
      if (pending == 0){
        render_chunk();
        pending = CHUNK_BYTES;
      }
      int done = CHUNK_BYTES - pending;
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
