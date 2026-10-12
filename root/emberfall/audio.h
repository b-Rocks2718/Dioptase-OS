/* Emberfall audio: chiptune music plus synthesized effects.
 *
 * The 25 kHz mono device holds about 0.33 s. The mixer renders small slices
 * every game tick: a looping bass-plus-lead tune and one-shot effects with
 * exponential decays. All synthesis is integer math with no tables.
 */

#ifndef EMBER_AUDIO_H
#define EMBER_AUDIO_H

/* Claim the device and start silent; safe to call once. */
void audio_init(void);

/* Release the device. */
void audio_shutdown(void);

/* Render one slice: music when unmuted plus the requested effects. */
void audio_tick(int sound_bits, int muted, int phase_playing);

#endif
