#include "skyforge.h"
#include "music_data.h"
#ifndef SKYFORGE_HOST_TEST
#include "../crt/sys.h"
#include "../crt/unistd.h"
#endif

/*
 * SKYFORGE 8-channel 25 kHz mu-law sample-playback tracker, 4-voice wavetable
 * sound-effect synthesizer, and dual-comb + allpass reverb running in a
 * dedicated child process on CORE_AUDIO.
 */

#define CMD_SONG_BASE 64
#define CMD_HEARTBEAT 0xFE
#define CMD_QUIT      0xFF

#define CTRL_SAMPLES 125
#define CHUNK_SAMPLES 250
#define CHUNK_BYTES   500

#define BUFFER_MIN 5000
#define BUFFER_MAX 13000
#define BUFFER_GROW 1500
#define BUFFER_SHRINK 250
#define BUFFER_STARVED 1000
#define BUFFER_CALM_FRAMES 600
#define SYNTH_SLEEP_JIFFIES 6
#define HEARTBEAT_PERIOD 30
#define HEARTBEAT_TIMEOUT 300
#define HEARTBEAT_CHECK_LOOPS 16

#define ENV_FULL 16384
#define PITCH_CENTER 64
#define LEVELS 64
#define VOL_SHIFT 8
#define MASTER_FULL 64
#define TAIL_RELEASE 4096

#define EV_NOTE 0x00
#define EV_OFF  0x08
#define EV_VOL  0x10
#define EV_WAIT 0x80
#define EV_END  0xFF

#define STAGE_ATTACK  0
#define STAGE_DECAY   1
#define STAGE_SUSTAIN 2
#define STAGE_RELEASE 3

/* Synthesize `samples` of test PCM into `out_pcm` for headless/host verification. */
void audio_synth_test_chunk(short* out_pcm, int samples){
  unsigned phase = 0;
  unsigned step = 55000000u;
  for (int i = 0; i < samples; ++i){
    phase += step;
    int wave = (phase & 0x80000000u) ? 6400 : -6400;
    out_pcm[i] = (short)wave;
  }
}

#ifndef SKYFORGE_HOST_TEST

static int cmd_fd = -1;
static int child_desc = -1;
static bool muted = false;
static int active_song = SONG_TITLE;
static unsigned last_heartbeat = 0;

static void send_byte(int value){
  if (cmd_fd < 0) return;
  unsigned char b = (unsigned char)value;
  write(cmd_fd, &b, 1);
}

/* Trigger sound effect `sfx_id` (1..12) unless audio is muted. */
void sfx_trigger(int sfx_id){
  if (!muted && sfx_id > 0 && sfx_id < 64){
    send_byte(sfx_id);
  }
}

/* Switch background music to `song_id` (SONG_*). */
void audio_set_song(int song_id){
  active_song = song_id;
  if (!muted){
    send_byte(CMD_SONG_BASE + song_id);
  }
}

/* Toggle audio mute state and return the new mute status. */
bool audio_toggle_mute(void){
  muted = !muted;
  send_byte(CMD_SONG_BASE + (muted ? SONG_NONE : active_song));
  return muted;
}

/* Send periodic keepalive heartbeat to the synthesizer process. */
void audio_heartbeat(void){
  if (cmd_fd < 0) return;
  unsigned now = get_vga_frame_counter();
  if (now - last_heartbeat >= HEARTBEAT_PERIOD){
    last_heartbeat = now;
    send_byte(CMD_HEARTBEAT);
  }
}

/* Shut down and reap the synthesizer process on exit. */
void audio_stop(void){
  if (cmd_fd < 0) return;
  send_byte(CMD_QUIT);
  close(cmd_fd);
  cmd_fd = -1;
  if (child_desc >= 0){
    wait_child(child_desc);
    child_desc = -1;
  }
}

static void on_terminate(int signal){
  (void)signal;
  send_byte(CMD_QUIT);
  exit(0);
}

/* ---- Synthesizer Child Process State ---- */

static int mix_acc[CHUNK_SAMPLES];
static short out_buf[CHUNK_SAMPLES];

struct MusicVoice {
  unsigned char* data;
  unsigned pos;
  unsigned step;
  unsigned end;
  unsigned loop_len;
  int env;
  int stage;
  int attack;
  int decay;
  int sustain;
  int release;
  int vel;
  int level;
};

static struct MusicVoice music_voices[MUSIC_CHANNELS];
static struct MusicVoice tail_voices[MUSIC_CHANNELS];
static int chan_vol[MUSIC_CHANNELS];

#define VOL_TABLE_BYTES 32768
static short* vol_table;
static unsigned char scale6[4160];

static unsigned char* music_samples;
static int music_warm_offset;
static unsigned music_touch_sum;
#define WARM_PAGES_PER_LOOP 2

static int song = SONG_NONE;
static int queued_song = SONG_NONE;
static bool fading = false;
static int master = MASTER_FULL;
static int row_samples;
static int row_left;
static int rows_left;
static unsigned char* events;
static int order_first;
static int order_count;
static int order_index;
static int order_loop;

static bool load_music_samples(void){
  int fd = open_existing(MUSIC_FILE);
  if (fd < 0){
    fd = open_existing("music.dat");
  }
  if (fd < 0) return false;
  unsigned char* mem = (unsigned char*)mmap(MUSIC_SAMPLE_BYTES, fd, 0, MMAP_READ);
  close(fd);
  if ((int)mem == -1) return false;
  music_samples = mem;
  music_warm_offset = 0;
  return true;
}

static bool warm_music_pages(void){
  if (music_samples == NULL || music_warm_offset >= MUSIC_SAMPLE_BYTES) return false;
  volatile unsigned char* bytes = music_samples;
  for (int i = 0; i < WARM_PAGES_PER_LOOP && music_warm_offset < MUSIC_SAMPLE_BYTES; ++i){
    music_touch_sum += bytes[music_warm_offset];
    music_warm_offset += 4096;
  }
  return true;
}

static void build_music_tables(void){
  for (int code = 0; code < 256; ++code){
    int linear = mulaw_table[code];
    int sum = 0;
    for (int level = 0; level < LEVELS; ++level){
      vol_table[(level << 8) + code] = (short)(sum >> VOL_SHIFT);
      sum += linear;
    }
  }
  for (int a = 0; a <= 64; ++a){
    int sum = 0;
    for (int b = 0; b < 64; ++b){
      scale6[(a << 6) + b] = (unsigned char)(sum >> 6);
      sum += a;
    }
  }
  for (int c = 0; c < MUSIC_CHANNELS; ++c) chan_vol[c] = 63;
}

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

static void music_silence(void){
  for (int c = 0; c < MUSIC_CHANNELS; ++c){
    music_voices[c].data = NULL;
    tail_voices[c].data = NULL;
    chan_vol[c] = 63;
  }
}

static void music_load_pattern(void){
  int pattern = music_orders[order_first + order_index];
  events = music_events + music_pattern_offsets[pattern];
}

static void music_start(int id){
  music_silence();
  song = id;
  master = MASTER_FULL;
  if (id <= SONG_NONE || id > MUSIC_SONGS || music_samples == NULL){
    song = SONG_NONE;
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

static void music_request(int id){
  if (id == song && !fading) return;
  if (song == SONG_NONE){
    music_start(id);
    return;
  }
  queued_song = id;
  fading = true;
}

static void music_sequence(void){
  if (song == SONG_NONE) return;
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
            for (int c = 0; c < MUSIC_CHANNELS; ++c) music_voices[c].stage = STAGE_RELEASE;
            song = SONG_NONE;
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

static void voice_envelope(struct MusicVoice* v, int cvol){
  if (v->data == NULL) return;
  if (v->stage == STAGE_ATTACK){
    v->env += v->attack;
    if (v->env >= ENV_FULL){
      v->env = ENV_FULL;
      v->stage = STAGE_DECAY;
    }
  } else if (v->stage == STAGE_DECAY){
    v->env -= v->decay;
    if (v->env <= v->sustain){
      v->env = v->sustain;
      v->stage = STAGE_SUSTAIN;
    }
  } else if (v->stage == STAGE_RELEASE){
    v->env -= v->release;
    if (v->env <= 0){
      v->data = NULL;
      return;
    }
  }
  int vv = scale6[(cvol << 6) + v->vel];
  int level = scale6[((v->env >> 8) << 6) + vv];
  v->level = scale6[(master << 6) + level];
}

static void music_control(void){
  if (fading){
    master -= 2;
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

static void mix_voice(struct MusicVoice* v){
  if (v->data == NULL || v->level <= 0) return;
  unsigned pos = mix_sample(mix_acc, CHUNK_SAMPLES, v->data, v->pos, v->step,
                            v->end, v->loop_len, vol_table + (v->level << 8));
  if (pos >= v->end) v->data = NULL;
  else v->pos = pos;
}

/* ---- 4-Voice Wavetable SFX Synthesizer ---- */

#define WAVE_SQUARE   0
#define WAVE_PULSE    1
#define WAVE_TRIANGLE 2
#define WAVE_SAW      3
#define WAVE_SINE     4
#define WAVE_NOISE    5
#define WAVE_TABLES   5
#define WAVE_STEPS    64
#define VOLUMES       16
#define AMP_STEP      520
#define SFX_VOICES    4
#define SFX_IDS       13

struct Voice {
  int on;
  unsigned phase;
  unsigned inc;
  int wave;
  int vol;
  int vol_step;
  unsigned lfsr;
  int noise_out;
  int seg;
  int seg_ticks;
  int inc_step;
};

static struct Voice sfx_voices[SFX_VOICES];
static short wavetab[5120];

struct SfxSeg {
  int wave;
  int hz0;
  int hz1;
  int ms;
  int vol0;
  int vol1;
  int last;
};

static struct SfxSeg sfx_segs[20] = {
  /* 0 PLASMA */         { WAVE_PULSE,    1600,  420,  85, 11, 0, 1 },
  /* 1 SEISMIC_LAUNCH */ { WAVE_SAW,       520,  160, 180, 13, 6, 0 },
  /* 2 */                { WAVE_NOISE,     900,  220, 220, 10, 0, 1 },
  /* 3 CRATER_BOOM */    { WAVE_NOISE,    1600,  180, 140, 15, 12, 0 },
  /* 4 */                { WAVE_NOISE,     260,   55, 480, 13, 0, 1 },
  /* 5 RING_PASS */      { WAVE_SINE,     1047, 1319,  75, 12, 9, 0 },
  /* 6 */                { WAVE_SINE,     1568, 2093, 180, 12, 0, 1 },
  /* 7 BARREL_ROLL */    { WAVE_TRIANGLE,  320, 1100, 110, 12, 9, 0 },
  /* 8 */                { WAVE_SINE,     1100,  480, 140, 10, 0, 1 },
  /* 9 ENEMY_HIT */      { WAVE_SQUARE,    620,  220, 110, 10, 0, 1 },
  /* 10 ENEMY_EXPLODE */ { WAVE_NOISE,    1400,  240, 130, 14, 10, 0 },
  /* 11 */               { WAVE_SAW,       240,   60, 340, 11, 0, 1 },
  /* 12 PLAYER_HURT */   { WAVE_SAW,       280,  100, 220, 14, 0, 1 },
  /* 13 BEACON_PICKUP */ { WAVE_TRIANGLE,  523, 1047,  90, 11, 8, 0 },
  /* 14 */               { WAVE_TRIANGLE, 1047, 1568, 180, 11, 0, 1 },
  /* 15 SCULPT_HUM */    { WAVE_SINE,      340,  460,  70,  7, 0, 1 },
  /* 16 SECTOR_CLEAR */  { WAVE_SQUARE,    523,  659, 110, 11, 9, 0 },
  /* 17 */               { WAVE_SQUARE,    784, 1047, 260, 12, 0, 1 },
  /* 18 BOSS_ALARM */    { WAVE_SAW,       220,  440, 220, 13, 9, 0 },
  /* 19 */               { WAVE_SAW,       440,  180, 320, 13, 0, 1 }
};

static int sfx_start[SFX_IDS] = {
  -1, 0, 1, 3, 5, 7, 9, 10, 12, 13, 15, 16, 18
};

static void build_wavetables(void){
  for (int w = 0; w < WAVE_TABLES; ++w){
    for (int v = 0; v < VOLUMES; ++v){
      int amp = v * AMP_STEP;
      for (int i = 0; i < WAVE_STEPS; ++i){
        int s = 0;
        if (w == WAVE_SQUARE) s = i < 32 ? 1024 : -1024;
        else if (w == WAVE_PULSE) s = i < 16 ? 1024 : -1024;
        else if (w == WAVE_TRIANGLE) s = i < 32 ? -1024 + i * 64 : 1024 - (i - 32) * 64;
        else if (w == WAVE_SAW) s = -1024 + i * 32;
        else s = sin_lut[(i << 4) & ANGLE_MASK] >> 6;
        wavetab[(w * VOLUMES + v) * WAVE_STEPS + i] = (short)((s * amp) >> 10);
      }
    }
  }
}

static unsigned hz_inc(int hz){
  return (unsigned)hz * 171799u;
}

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

static int next_sfx_voice = 0;

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

static void mix_sfx(void){
  for (int k = 0; k < SFX_VOICES; ++k){
    struct Voice* v = &sfx_voices[k];
    if (!v->on) continue;
    int level = v->vol >> 16;
    if (level <= 0) continue;
    unsigned phase = v->phase;
    unsigned inc = v->inc;
    if (v->wave == WAVE_NOISE){
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

/* ---- Reverb and Output Loop ---- */

#define COMB1_LEN 3511
#define COMB2_LEN 4813
#define ALLPASS_LEN 577
#define REVERB_BYTES 35604
static struct Reverb reverb;

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

static void synth_main(int fd){
  if (audio_open() < 0){
    while (true){
      unsigned char c;
      if (read(fd, &c, 1) != 1 || c == CMD_QUIT) exit(0);
    }
  }
  request_priority(DIOPTASE_PRIORITY_HIGH);
  pin_core(CORE_AUDIO);
  if (!synth_alloc()){
    audio_close();
    while (true){
      unsigned char c;
      if (read(fd, &c, 1) != 1 || c == CMD_QUIT) exit(0);
    }
  }
  build_wavetables();
  build_music_tables();
  load_music_samples();
  music_start(SONG_TITLE);

  unsigned last_alive = get_vga_frame_counter();
  unsigned calm_since = last_alive;
  int target = BUFFER_MIN;
  int pending = 0;
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
        if (c == CMD_HEARTBEAT) continue;
        if (c >= CMD_SONG_BASE) music_request(c - CMD_SONG_BASE);
        else start_sfx(c);
      }
      loops = 0;
      last_alive = get_vga_frame_counter();
    } else if ((loops & (HEARTBEAT_CHECK_LOOPS - 1)) == 0 &&
               get_vga_frame_counter() - last_alive > HEARTBEAT_TIMEOUT){
      audio_close();
      exit(0);
    }

    bool warming = warm_music_pages();
    int buffered = audio_buffered();
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
      int accepted = audio_write((char*)out_buf + done, (unsigned)pending);
      if (accepted <= 0) break;
      pending -= accepted;
      buffered += accepted;
    }
    sleep(SYNTH_SLEEP_JIFFIES);
  }
}

void audio_start(bool silent){
  if (silent){
    muted = true;
    return;
  }
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
  child_desc = pid;
  cmd_fd = fds[1];
  last_heartbeat = get_vga_frame_counter();
  register_handler(SIGNAL_TERMINATE, (void*)on_terminate);
}

#else
void sfx_trigger(int sfx_id){ (void)sfx_id; }
void audio_set_song(int song_id){ (void)song_id; }
#endif /* !SKYFORGE_HOST_TEST */
