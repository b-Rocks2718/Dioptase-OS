#include "music.h"
#include "game.h"

/*
 * Original score, "Afterlight": a minor ninth arpeggio through four harmonies,
 * with bass, a beating fifth, restrained percussion, and a recirculating echo.
 * Its movements are Transit Glass (D minor), Quiet Circuit (A minor), and
 * The Last Signal (E minor). The 16-bar figure takes roughly forty seconds.
 * Sector changes select another key and tempo on the next eighth note.
 *
 * This module executes in user mode and touches only its own RAM. The ABI's
 * Types section specifies 32-bit unsigned phase accumulators and 16-bit short
 * PCM samples. Unsigned addition deliberately wraps; all bit shifts operate
 * on nonnegative values. No TLS, register allocation, stack layout, interrupt,
 * MMIO, or multicore behavior is assumed. The foreground thread owns all state.
 */

/* The power-of-two echo is about 328 ms at the authored sample rate. */
#define ECHO_SAMPLES 8192
/* Envelopes run at roughly 391 Hz instead of doing control work every sample. */
#define CONTROL_SAMPLES 64
/* Each level adds 64 PCM units; every authored voice stays below 4096 units. */
#define VOLUME_STEPS 64
/* Leave headroom for the OS output path while keeping the PCM cast in range. */
#define MIX_LIMIT 16000

/* One offset sine period: 1024 is silence, and the extrema are 0 and 2048. */
static const unsigned short sine_wave[256] = {
    1024, 1049, 1074, 1099, 1124, 1149, 1174, 1199, 1224, 1248, 1273, 1297, 1321, 1345, 1369, 1393,
    1416, 1439, 1462, 1484, 1507, 1529, 1550, 1572, 1593, 1614, 1634, 1654, 1674, 1693, 1712, 1730,
    1748, 1766, 1783, 1799, 1816, 1831, 1846, 1861, 1875, 1889, 1902, 1915, 1927, 1939, 1950, 1960,
    1970, 1979, 1988, 1996, 2004, 2011, 2017, 2023, 2028, 2033, 2037, 2040, 2043, 2045, 2047, 2048,
    2048, 2048, 2047, 2045, 2043, 2040, 2037, 2033, 2028, 2023, 2017, 2011, 2004, 1996, 1988, 1979,
    1970, 1960, 1950, 1939, 1927, 1915, 1902, 1889, 1875, 1861, 1846, 1831, 1816, 1799, 1783, 1766,
    1748, 1730, 1712, 1693, 1674, 1654, 1634, 1614, 1593, 1572, 1550, 1529, 1507, 1484, 1462, 1439,
    1416, 1393, 1369, 1345, 1321, 1297, 1273, 1248, 1224, 1199, 1174, 1149, 1124, 1099, 1074, 1049,
    1024, 999, 974, 949, 924, 899, 874, 849, 824, 800, 775, 751, 727, 703, 679, 655,
    632, 609, 586, 564, 541, 519, 498, 476, 455, 434, 414, 394, 374, 355, 336, 318,
    300, 282, 265, 249, 232, 217, 202, 187, 173, 159, 146, 133, 121, 109, 98, 88,
    78, 69, 60, 52, 44, 37, 31, 25, 20, 15, 11, 8, 5, 3, 1, 0,
    0, 0, 1, 3, 5, 8, 11, 15, 20, 25, 31, 37, 44, 52, 60, 69,
    78, 88, 98, 109, 121, 133, 146, 159, 173, 187, 202, 217, 232, 249, 265, 282,
    300, 318, 336, 355, 374, 394, 414, 434, 455, 476, 498, 519, 541, 564, 586, 609,
    632, 655, 679, 703, 727, 751, 775, 800, 824, 849, 874, 899, 924, 949, 974, 999
};

/* C2..B5: round(frequency * 2^32 / 25000), equal temperament at A4=440 Hz. */
static const unsigned notes[48] = {
    11236732u, 11904903u, 12612806u, 13362802u, 14157396u, 14999238u,
    15891139u, 16836076u, 17837201u, 18897856u, 20021581u, 21212126u,
    22473465u, 23809807u, 25225611u, 26725604u, 28314792u, 29998477u,
    31782279u, 33672152u, 35674402u, 37795712u, 40043162u, 42424253u,
    44946930u, 47619613u, 50451223u, 53451209u, 56629583u, 59996953u,
    63564558u, 67344303u, 71348804u, 75591424u, 80086324u, 84848505u,
    89893860u, 95239227u, 100902446u, 106902418u, 113259166u, 119993907u,
    127129116u, 134688606u, 142697608u, 151182849u, 160172649u, 169697010u
};

/* D minor, A minor, then E minor: each key visits VI, III, and VII. */
static const int chord_roots[12] = {2, 10, 5, 0, 9, 5, 0, 7, 4, 0, 7, 2};
static const int arp_intervals[8] = {0, 7, 14, 19, 10, 7, 3, 14};
/* Eighth-note lengths approximate 96, 104, and 112 beats per minute. */
static const int step_samples[3] = {7812, 7212, 6696};
static const int chime_notes[4] = {26, 29, 33, 38};

static unsigned bass_phase;
static unsigned pad_phase;
static unsigned shimmer_phase;
static unsigned arp_phase;
static unsigned kick_phase;
static unsigned effect_phase;
static unsigned bass_inc;
static unsigned pad_inc;
static unsigned shimmer_inc;
static unsigned arp_inc;
static unsigned kick_inc;
static unsigned effect_inc;
static unsigned arp_amp;
static unsigned kick_amp;
static unsigned noise_amp;
static unsigned effect_amp;
static unsigned effect_noise;
static unsigned noise_state;
static unsigned fade;
static unsigned bass_slot;
static unsigned pad_slot;
static unsigned shimmer_slot;
static unsigned arp_slot;
static unsigned kick_slot;
static unsigned effect_slot;
static unsigned noise_slot;
static int sequence_step;
static int step_left;
static int control_left;
static int effect_kind;
static int effect_ticks;
static int effect_age;
static int echo_cursor;
static short echo[ECHO_SAMPLES];
static int wave_ready;
/* 64 gain levels times 256 phases; bcc requires a literal array extent. */
static short scaled_wave[16384];

/* Build quantized gains once using addition, avoiding software multiply in PCM. */
static void music_waves(void) {
    int phase;
    int level;
    unsigned accumulated;
    unsigned value;
    if (wave_ready) return;
    for (phase = 0; phase < 256; phase++) {
        value = sine_wave[phase];
        accumulated = 0;
        for (level = 0; level < VOLUME_STEPS; level++) {
            scaled_wave[(level << 8) + phase] =
                (short)((int)(accumulated >> 4) - (level << 6));
            accumulated += value;
        }
    }
    wave_ready = 1;
}

/* Reset every sample-dependent state so a replay starts with identical audio. */
void music_reset(void) {
    int i;
    music_waves();
    bass_phase = 0;
    pad_phase = 0;
    shimmer_phase = 0;
    arp_phase = 0;
    kick_phase = 0;
    effect_phase = 0;
    bass_inc = 0;
    pad_inc = 0;
    shimmer_inc = 0;
    arp_inc = 0;
    kick_inc = 0;
    effect_inc = 0;
    arp_amp = 0;
    kick_amp = 0;
    noise_amp = 0;
    effect_amp = 0;
    effect_noise = 0;
    noise_state = 0xACE1u;
    fade = 0;
    bass_slot = 0;
    pad_slot = 0;
    shimmer_slot = 0;
    arp_slot = 0;
    kick_slot = 0;
    effect_slot = 0;
    noise_slot = 0;
    sequence_step = -1;
    step_left = 0;
    control_left = 0;
    effect_kind = SOUND_NONE;
    effect_ticks = 0;
    effect_age = 0;
    echo_cursor = 0;
    for (i = 0; i < ECHO_SAMPLES; i++) echo[i] = 0;
}

/* Set pitches on eighth-note boundaries; oscillator phases remain continuous. */
static void music_step(int sector) {
    int beat;
    int chord;
    int root;
    int interval;
    int note;
    sequence_step = (sequence_step + 1) & 127;
    beat = sequence_step & 7;
    chord = (sequence_step >> 3) & 3;
    root = chord_roots[sector * 4 + chord];
    interval = arp_intervals[beat];
    if (chord != 0 && (beat == 4 || beat == 6)) interval++;
    note = root + 12 + interval;
    /* Every other four-bar phrase drops selected notes into a gentler octave. */
    if ((sequence_step & 32) && (beat & 1)) note -= 12;
    bass_inc = notes[root];
    pad_inc = notes[root + 19];
    shimmer_inc = pad_inc + (pad_inc >> 9);
    arp_inc = notes[note];
    arp_amp = (beat & 1) ? 1500 : 2000;
    if (beat == 0 || beat == 4) {
        kick_phase = 0;
        kick_inc = notes[14];
        kick_amp = 1000;
    }
    if ((beat & 1) || sector == 2) noise_amp = 130;
    if (beat == 2 || beat == 6) noise_amp = 300;
    step_left = step_samples[sector];
}

/* Replace short action cues, preserving a rewarding chime across footsteps. */
void music_effect(int sound) {
    if (sound <= SOUND_NONE || sound > SOUND_EXIT) return;
    if (sound == SOUND_STEP && effect_ticks > 0) return;
    effect_kind = sound;
    effect_phase = 0;
    effect_age = 0;
    effect_noise = 0;
    effect_amp = 0;
    effect_ticks = 50;
    effect_inc = notes[26];
    if (sound == SOUND_STEP) {
        effect_noise = 210;
        effect_ticks = 12;
    } else if (sound == SOUND_SHOT) {
        effect_inc = notes[47];
        effect_amp = 2300;
        effect_noise = 450;
    } else if (sound == SOUND_HIT) {
        effect_inc = notes[12];
        effect_amp = 1500;
        effect_noise = 1400;
    } else if (sound == SOUND_HURT) {
        effect_inc = notes[0];
        effect_amp = 2400;
        effect_noise = 600;
        effect_ticks = 95;
    } else {
        effect_amp = 2300;
        effect_ticks = 180;
        if (sound == SOUND_PICKUP) effect_ticks = 80;
        if (sound == SOUND_EXIT) effect_ticks = 260;
    }
}

/* Advance envelopes and swept tones at a fixed cadence independent of chunks. */
static void music_control(void) {
    arp_amp -= (arp_amp + 31) >> 5;
    kick_amp -= (kick_amp + 15) >> 4;
    noise_amp -= (noise_amp + 7) >> 3;
    if (kick_inc > notes[0] / 2) kick_inc -= kick_inc >> 5;
    if (fade < 1024) fade += 16;
    if (effect_ticks > 0) {
        effect_ticks--;
        effect_age++;
        effect_amp -= (effect_amp + 31) >> 5;
        effect_noise -= (effect_noise + 7) >> 3;
        if (effect_kind == SOUND_SHOT) effect_inc -= effect_inc >> 5;
        if (effect_kind == SOUND_HURT) effect_inc -= effect_inc >> 8;
        if (effect_kind == SOUND_RELAY || effect_kind == SOUND_PICKUP ||
            effect_kind == SOUND_EXIT) {
            if ((effect_age & 31) == 0 && effect_age <= 96) {
                effect_inc = notes[chime_notes[effect_age >> 5]];
                effect_amp = 2200;
            }
        }
        if (effect_ticks == 0) {
            effect_amp = 0;
            effect_noise = 0;
        }
    }
    /* Gain quantization and fade happen only here; PCM needs no multiplication.
     * The >>16 combines fade/1024 and amplitude/64 before indexing a waveform.
     * Each resulting level is below VOLUME_STEPS by the authored gain bounds.
     */
    bass_slot = ((1000u * fade) >> 16) << 8;
    pad_slot = ((440u * fade) >> 16) << 8;
    shimmer_slot = ((360u * fade) >> 16) << 8;
    arp_slot = ((arp_amp * fade) >> 16) << 8;
    kick_slot = ((kick_amp * fade) >> 16) << 8;
    effect_slot = ((effect_amp * fade) >> 16) << 8;
    noise_slot = (((noise_amp + effect_noise) * fade) >> 16) << 8;
    control_left = CONTROL_SAMPLES;
}

/* Mix cached waveform gains and one echo with additions, shifts, and lookups. */
void music_render(short* dst, int samples, int sector) {
    int i;
    int mix;
    int arp;
    int effect;
    int delayed;
    if (sector < 0) sector = 0;
    if (sector > 2) sector = 2;
    for (i = 0; i < samples; i++) {
        if (step_left == 0) music_step(sector);
        if (control_left == 0) music_control();
        step_left--;
        control_left--;
        bass_phase += bass_inc;
        pad_phase += pad_inc;
        shimmer_phase += shimmer_inc;
        arp_phase += arp_inc;
        kick_phase += kick_inc;
        effect_phase += effect_inc;
        /* A maximal-length 16-bit LFSR gives quiet deterministic percussion. */
        noise_state = (noise_state >> 1) ^ ((0u - (noise_state & 1u)) & 0xB400u);
        mix = scaled_wave[bass_slot + (bass_phase >> 24)];
        mix += scaled_wave[pad_slot + (pad_phase >> 24)];
        mix += scaled_wave[shimmer_slot + (shimmer_phase >> 24)];
        arp = scaled_wave[arp_slot + (arp_phase >> 24)];
        effect = scaled_wave[effect_slot + (effect_phase >> 24)];
        mix += scaled_wave[kick_slot + (kick_phase >> 24)];
        mix += scaled_wave[noise_slot + (noise_state & 255u)];
        delayed = echo[echo_cursor];
        /* Half feedback bounds the delay by twice its bounded input. */
        echo[echo_cursor] = (short)(arp + effect / 2 + delayed / 2);
        echo_cursor = (echo_cursor + 1) & (ECHO_SAMPLES - 1);
        mix += arp + effect + delayed / 2;
        /* /2 above is deliberate: signed right-shift semantics are not assumed. */
        if (mix > MIX_LIMIT) mix = MIX_LIMIT;
        if (mix < -MIX_LIMIT) mix = -MIX_LIMIT;
        dst[i] = (short)mix;
    }
}

/* Exercise reset, chunk boundaries, all sector moods, and every event voice. */
int music_selftest(void) {
    short first[128];
    short chunk[128];
    int i;
    int sector;
    int block;
    int audible;
    int result;
    result = 0;
    music_reset();
    music_render(first, 128, 0);
    music_reset();
    music_render(chunk, 39, 0);
    music_render(chunk + 39, 89, 0);
    for (i = 0; i < 128; i++) {
        if (first[i] != chunk[i]) result = 1;
    }
    for (sector = 0; sector < 3; sector++) {
        music_reset();
        audible = 0;
        for (block = 0; block < 160; block++) {
            if ((block & 15) == 0) music_effect((block >> 4) % 7 + 1);
            music_render(chunk, 128, sector);
            for (i = 0; i < 128; i++) {
                if (chunk[i] < -16000 || chunk[i] > 16000) result = 2;
                if (chunk[i] < -100 || chunk[i] > 100) audible = 1;
            }
        }
        if (!audible) result = 3;
    }
    music_reset();
    return result;
}
