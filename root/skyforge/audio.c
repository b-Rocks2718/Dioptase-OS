#include "skyforge.h"
#ifndef SKYFORGE_HOST_TEST
#include "../crt/sys.h"
#include "../crt/unistd.h"
#endif

/*
 * 25 kHz 16-bit mono PCM synthesizer running in a dedicated child process.
 * Synthesizes 5 multi-voice musical themes (lead, arpeggiator, bass, drums)
 * and 12 polyphonic sound effects using phase accumulators and bitshifts with
 * zero per-sample multiplications.
 */

#define CMD_SONG_BASE 64
#define CMD_HEARTBEAT 0xFE
#define CMD_QUIT      0xFF

#define CHUNK_SAMPLES 250
#define CHUNK_BYTES   500
#define TARGET_BUFFERED_BYTES 6000
#define HEARTBEAT_PERIOD_FRAMES 30
#define HEARTBEAT_TIMEOUT_FRAMES 300

#ifndef SKYFORGE_HOST_TEST
static int audio_pipe_wr = -1;
static int audio_child_desc = -1;
static bool audio_muted = false;
static unsigned last_beat_frame = 0;
#endif

/* 32-step MIDI-style pitch step table (25 kHz 32-bit phase increments). */
static unsigned note_steps[32] = {
  0,
  18919725u, 20044751u, 21236675u, 22499473u, 23837361u, 25254804u, 26756534u,
  28347542u, 30033177u, 31819042u, 33711101u, 35715669u, 37839450u, 40089502u,
  42473350u, 44998946u, 47674722u, 50509608u, 53513068u, 56695084u, 60066354u,
  63638084u, 67422202u, 71431338u, 75678900u, 80179004u, 84946700u, 89997892u,
  95349444u, 101019216u, 107026136u
};

/* Lead melodies (32 steps per pattern) for the 5 themes */
static unsigned char song_lead[160] = {
  /* SONG_TITLE: Heroic Skyforge Overture */
  13, 16, 18, 20, 25, 23, 20, 18, 16, 18, 20, 23, 25, 28, 27, 25,
  13, 16, 18, 20, 25, 23, 20, 18, 15, 18, 20, 22, 25,  0, 25,  0,
  /* SONG_EMERALD: Canyon Flight Adventure */
  15, 18, 20, 22, 27, 25, 22, 20, 18, 20, 22, 25, 27, 30, 27, 25,
  15, 18, 20, 22, 27, 25, 22, 20, 17, 20, 22, 24, 27, 25, 22, 20,
  /* SONG_CYBER: Synthwave Trench Drive */
  14,  0, 14, 17, 19, 21, 26, 24, 14,  0, 14, 17, 21, 19, 17, 16,
  12,  0, 12, 16, 19, 21, 24, 21, 14, 17, 19, 21, 26, 24, 21, 19,
  /* SONG_BOSS: Leviathan Dreadnought Battle */
  13, 14, 13, 11, 13, 16, 15, 13, 13, 14, 13, 11, 18, 17, 16, 14,
  16, 17, 16, 14, 16, 20, 18, 16, 13, 16, 18, 20, 25, 24, 20, 18,
  /* SONG_FORGE: Crystalline World Sculptor Ambient */
  18,  0, 20,  0, 22,  0, 25,  0, 23,  0, 20,  0, 18,  0, 16,  0,
  15,  0, 18,  0, 20,  0, 23,  0, 22,  0, 18,  0, 16,  0, 13,  0
};

/* Basslines (32 steps per pattern) for the 5 themes */
static unsigned char song_bass[160] = {
  /* SONG_TITLE */
  1, 1, 8, 1, 6, 6, 13, 6, 4, 4, 11, 4, 8, 8, 10, 8,
  1, 1, 8, 1, 6, 6, 13, 6, 4, 4, 11, 4, 8, 8,  1, 1,
  /* SONG_EMERALD */
  3, 3, 10, 3, 8, 8, 15, 8, 6, 6, 13, 6, 10, 10, 8, 6,
  3, 3, 10, 3, 8, 8, 15, 8, 5, 5, 12, 5, 10, 10, 3, 3,
  /* SONG_CYBER */
  2, 2, 14, 2, 2, 2, 14, 2, 7, 7, 19, 7, 5, 5, 17, 5,
  2, 2, 14, 2, 9, 9, 21, 9, 7, 7, 19, 7, 2, 2, 14, 2,
  /* SONG_BOSS */
  1, 8, 1, 8, 2, 9, 2, 9, 1, 8, 1, 8, 4, 11, 4, 11,
  1, 8, 1, 8, 6, 13, 6, 13, 4, 11, 3, 10, 1, 8, 1, 1,
  /* SONG_FORGE */
  6, 0, 6, 0, 4, 0, 4, 0, 1, 0, 1, 0, 3, 0, 3, 0,
  6, 0, 6, 0, 8, 0, 8, 0, 4, 0, 4, 0, 1, 0, 1, 0
};

/* Synthesizer state */
static int cur_song = SONG_TITLE;
static unsigned seq_tick = 0;
static unsigned seq_sample_rem = 0;
static unsigned phase_lead = 0;
static unsigned phase_arp = 0;
static unsigned phase_bass = 0;
static unsigned step_lead = 0;
static unsigned step_arp = 0;
static unsigned step_bass = 0;
static int env_lead = 0;
static int env_bass = 0;
static int env_drum = 0;
static unsigned noise_lfsr = 0xACE1u;

/* Active sound-effect voice state */
static int sfx_active = 0;
static unsigned sfx_phase = 0;
static unsigned sfx_step = 0;
static int sfx_step_delta = 0;
static int sfx_env = 0;
static int sfx_noise_mix = 0;

/* Configure the SFX synthesizer voice for `sfx_id`. */
static void start_sfx_voice(int sfx_id){
  sfx_active = sfx_id;
  sfx_phase = 0;
  if (sfx_id == SFX_PLASMA){
    sfx_step = 110000000u;
    sfx_step_delta = -2400000;
    sfx_env = 220;
    sfx_noise_mix = 0;
  } else if (sfx_id == SFX_SEISMIC_LAUNCH){
    sfx_step = 65000000u;
    sfx_step_delta = -900000;
    sfx_env = 250;
    sfx_noise_mix = 1;
  } else if (sfx_id == SFX_CRATER_BOOM || sfx_id == SFX_ENEMY_EXPLODE){
    sfx_step = 18000000u;
    sfx_step_delta = -250000;
    sfx_env = 255;
    sfx_noise_mix = 3;
  } else if (sfx_id == SFX_RING_PASS || sfx_id == SFX_BEACON_PICKUP){
    sfx_step = 75000000u;
    sfx_step_delta = 2200000;
    sfx_env = 230;
    sfx_noise_mix = 0;
  } else if (sfx_id == SFX_BARREL_ROLL){
    sfx_step = 42000000u;
    sfx_step_delta = 1400000;
    sfx_env = 210;
    sfx_noise_mix = 1;
  } else if (sfx_id == SFX_ENEMY_HIT){
    sfx_step = 55000000u;
    sfx_step_delta = -1800000;
    sfx_env = 190;
    sfx_noise_mix = 2;
  } else if (sfx_id == SFX_PLAYER_HURT){
    sfx_step = 28000000u;
    sfx_step_delta = -600000;
    sfx_env = 250;
    sfx_noise_mix = 2;
  } else if (sfx_id == SFX_SCULPT_HUM){
    sfx_step = 36000000u;
    sfx_step_delta = 500000;
    sfx_env = 140;
    sfx_noise_mix = 0;
  } else {
    sfx_step = 60000000u;
    sfx_step_delta = 1200000;
    sfx_env = 240;
    sfx_noise_mix = 0;
  }
}

/* Advance the 8 Hz step sequencer and envelopes once every 125 samples (200 Hz). */
static void control_tick(void){
  if (seq_sample_rem == 0){
    seq_sample_rem = 24; /* 24 control ticks = 3,000 samples (~120 ms per 16th note) */
    int s_idx = cur_song - 1;
    if (s_idx >= 0 && s_idx < 5){
      int step = (int)(seq_tick & 31);
      int l_note = song_lead[(s_idx << 5) | step];
      int b_note = song_bass[(s_idx << 5) | step];
      if (l_note > 0){
        step_lead = note_steps[l_note & 31];
        step_arp  = note_steps[(l_note + 4) & 31];
        env_lead  = 180;
      }
      if (b_note > 0){
        step_bass = note_steps[b_note & 31] >> 1;
        env_bass  = 200;
      }
      if ((step & 3) == 0 || (step & 7) == 6){
        env_drum = 160;
      }
    }
    seq_tick += 1;
  } else {
    seq_sample_rem -= 1;
  }

  if (env_lead > 4) env_lead -= 4; else env_lead = 0;
  if (env_bass > 5) env_bass -= 5; else env_bass = 0;
  if (env_drum > 12) env_drum -= 12; else env_drum = 0;

  if (sfx_active && sfx_env > 0){
    sfx_step = (unsigned)((int)sfx_step + sfx_step_delta);
    sfx_env -= 6;
    if (sfx_env <= 0){
      sfx_env = 0;
      sfx_active = 0;
    }
  }
}

/* Synthesize `samples` of 16-bit mono PCM into `out_pcm` without multiplications. */
void audio_synth_test_chunk(short* out_pcm, int samples){
  for (int i = 0; i < samples; ++i){
    if ((i & 127) == 0){
      control_tick();
    }
    int mix = 0;

    if (cur_song != SONG_NONE){
      phase_lead += step_lead;
      phase_arp  += step_arp;
      phase_bass += step_bass;

      /* Pulse lead + arpeggiator harmony */
      int lead_wave = (phase_lead & 0x80000000u) ? env_lead : -env_lead;
      int arp_wave  = (phase_arp  & 0x80000000u) ? (env_lead >> 1) : -(env_lead >> 1);
      /* Sawtooth-like stepped bass */
      int bass_wave = (int)((phase_bass >> 24) & 0xFF) - 128;
      if (env_bass == 0) bass_wave = 0;

      mix += (lead_wave + arp_wave + bass_wave) << 4;

      if (env_drum > 0){
        noise_lfsr ^= noise_lfsr << 13;
        noise_lfsr ^= noise_lfsr >> 17;
        noise_lfsr ^= noise_lfsr << 5;
        int d_wave = (noise_lfsr & 1) ? env_drum : -env_drum;
        mix += d_wave << 4;
      }
    }

    if (sfx_active && sfx_env > 0){
      sfx_phase += sfx_step;
      int s_wave = (sfx_phase & 0x80000000u) ? sfx_env : -sfx_env;
      if (sfx_noise_mix > 0){
        noise_lfsr ^= noise_lfsr << 13;
        noise_lfsr ^= noise_lfsr >> 17;
        noise_lfsr ^= noise_lfsr << 5;
        int n_wave = (noise_lfsr & 1) ? sfx_env : -sfx_env;
        s_wave = (s_wave + n_wave) >> 1;
      }
      mix += s_wave << 5;
    }

    if (mix > 28000) mix = 28000;
    if (mix < -28000) mix = -28000;
    out_pcm[i] = (short)mix;
  }
}

#ifndef SKYFORGE_HOST_TEST

/* Signal handler for SIGNAL_TERMINATE (Ctrl-C) in the parent game process. */
static void on_sig_terminate(int sig){
  (void)sig;
  if (audio_pipe_wr >= 0){
    unsigned char q = CMD_QUIT;
    write(audio_pipe_wr, &q, 1);
  }
  exit(0);
}

/* Main loop of the forked background synthesizer process. */
static void synth_process_loop(int pipe_rd){
  pin_core(CORE_AUDIO);
  if (audio_open() < 0){
    close(pipe_rd);
    exit(0);
  }

  short pcm_buf[CHUNK_SAMPLES];
  unsigned last_alive = get_vga_frame_counter();
  int loop_counter = 0;

  while (true){
    /* Drain all pending 1-byte commands from the parent game process */
    while (fd_bytes_available(pipe_rd) > 0){
      unsigned char cmd = 0;
      if (read(pipe_rd, &cmd, 1) != 1){
        audio_close();
        close(pipe_rd);
        exit(0);
      }
      last_alive = get_vga_frame_counter();
      if (cmd == CMD_QUIT){
        audio_close();
        close(pipe_rd);
        exit(0);
      } else if (cmd == CMD_HEARTBEAT){
        /* Heartbeat updated above */
      } else if (cmd >= CMD_SONG_BASE && cmd < CMD_SONG_BASE + 8){
        cur_song = cmd - CMD_SONG_BASE;
      } else if (cmd >= 1 && cmd <= 32){
        start_sfx_voice((int)cmd);
      }
    }

    /* Check parent heartbeat every 16 iterations */
    loop_counter += 1;
    if ((loop_counter & 15) == 0){
      if (get_vga_frame_counter() - last_alive > HEARTBEAT_TIMEOUT_FRAMES){
        audio_close();
        close(pipe_rd);
        exit(0);
      }
    }

    /* Top up the hardware audio ring buffer toward TARGET_BUFFERED_BYTES */
    int buffered = audio_buffered();
    if (buffered < 0) break;
    while (buffered < TARGET_BUFFERED_BYTES){
      audio_synth_test_chunk(pcm_buf, CHUNK_SAMPLES);
      int off = 0;
      while (off < CHUNK_BYTES){
        int wr = audio_write(((char*)pcm_buf) + off, (unsigned)(CHUNK_BYTES - off));
        if (wr <= 0) break;
        off += wr;
      }
      buffered += off;
      if (off < CHUNK_BYTES) break;
    }

    sleep(5);
  }

  audio_close();
  close(pipe_rd);
  exit(0);
}

/* Fork the background synthesizer process unless `silent` is true. */
void audio_start(bool silent){
  if (silent){
    audio_muted = true;
    return;
  }
  int fds[2];
  if (pipe(fds) < 0) return;
  int child = fork();
  if (child == 0){
    close(fds[1]);
    synth_process_loop(fds[0]);
    exit(0);
  }
  close(fds[0]);
  if (child < 0){
    close(fds[1]);
    return;
  }
  audio_pipe_wr = fds[1];
  audio_child_desc = child;
  last_beat_frame = get_vga_frame_counter();
  register_handler(SIGNAL_TERMINATE, (void*)on_sig_terminate);
}

/* Send a 1-byte command to the synthesizer process if running. */
static void send_audio_cmd(int cmd){
  if (audio_pipe_wr < 0) return;
  unsigned char b = (unsigned char)cmd;
  write(audio_pipe_wr, &b, 1);
}

/* Trigger sound effect `sfx_id` unless muted. */
void sfx_trigger(int sfx_id){
  start_sfx_voice(sfx_id);
  if (!audio_muted){
    send_audio_cmd(sfx_id);
  }
}

/* Switch background music to `song_id`. */
void audio_set_song(int song_id){
  cur_song = song_id;
  if (!audio_muted){
    send_audio_cmd(CMD_SONG_BASE + song_id);
  }
}

/* Toggle audio mute on/off and return the new mute status. */
bool audio_toggle_mute(void){
  audio_muted = !audio_muted;
  send_audio_cmd(CMD_SONG_BASE + (audio_muted ? SONG_NONE : cur_song));
  return audio_muted;
}

/* Send periodic keepalive heartbeat to the synthesizer child process. */
void audio_heartbeat(void){
  if (audio_pipe_wr < 0) return;
  unsigned now = get_vga_frame_counter();
  if (now - last_beat_frame >= HEARTBEAT_PERIOD_FRAMES){
    last_beat_frame = now;
    send_audio_cmd(CMD_HEARTBEAT);
  }
}

/* Shut down and reap the synthesizer child process on game exit. */
void audio_stop(void){
  if (audio_pipe_wr < 0) return;
  send_audio_cmd(CMD_QUIT);
  close(audio_pipe_wr);
  audio_pipe_wr = -1;
  if (audio_child_desc >= 0){
    wait_child(audio_child_desc);
    audio_child_desc = -1;
  }
}

#else
/* Host test stubs */
void sfx_trigger(int sfx_id){ start_sfx_voice(sfx_id); }
void audio_set_song(int song_id){ cur_song = song_id; }
#endif /* !SKYFORGE_HOST_TEST */
