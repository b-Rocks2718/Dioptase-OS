#ifndef STARFALL_GRAPHICS_H
#define STARFALL_GRAPHICS_H

#include "starfall.h"

/* Claim the documented tile and sprite devices and install Starfall's art. */
bool graphics_init(void);

/* Refresh only changed arena cells and periodically update the side panel. */
void graphics_draw(struct Game* game, bool paused);

#endif
