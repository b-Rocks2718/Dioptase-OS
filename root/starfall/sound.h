#ifndef STARFALL_SOUND_H
#define STARFALL_SOUND_H

#include "starfall.h"

/* Claim the optional PCM device for procedural music and effects. */
void sound_init(void);

/* Feed the device while keeping gameplay independent of audio availability. */
void sound_tick(struct Game* game);

/* Release the audio device before the user program exits. */
void sound_shutdown(void);

#endif
