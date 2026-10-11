#ifndef LUMEN_MUSIC_H
#define LUMEN_MUSIC_H

/* The score and oscillator tables are authored for this mono PCM sample rate. */
#define MUSIC_SAMPLE_RATE 25000

/* Restart the score, effect voice, and echo; call before rendering a new run. */
void music_reset(void);
/* Replace the effect voice with a SOUND_* cue; SOUND_NONE leaves it alone. */
void music_effect(int sound);
/*
 * Fill caller-owned signed 16-bit mono PCM storage for zero-based sector 0..2.
 * Values outside that range select the nearest sector; nonpositive counts do
 * nothing. The caller retains the buffer and handles device submission. All
 * APIs share state and must run on one user thread, with no concurrent calls.
 */
void music_render(short* dst, int samples, int sector);
/* Check deterministic playback and bounded, audible sector cues; resets state. */
int music_selftest(void);

#endif
