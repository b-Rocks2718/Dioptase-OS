#ifndef LUMEN_RENDER_H
#define LUMEN_RENDER_H

#include "game.h"

#define VIEW_W 160
#define VIEW_H 96
#define HUD_W 320
#define HUD_H 32
#define SCREEN_GAME 0
#define SCREEN_MAP 1
#define SCREEN_HELP 2
#define SCREEN_PAUSE 3

/* RAM images use the documented VGA 0x0BGR pixel format, without transparency.
 * Literal extents are VIEW_W*VIEW_H and HUD_W*HUD_H; bcc requires literals. */
extern unsigned short view_pixels[15360];
extern unsigned short hud_pixels[10240];

/* Prepare procedural textures once; no assets or display mappings are required. */
void render_init(void);
/* Render the world and UI with Q8 camera coordinates and a 256-step full turn. */
void render_frame(struct Game* game, int camera_x, int camera_y,
                  int angle, unsigned frame, int screen);
/* Validate raster clipping and deterministic rendering without device access. */
int render_selftest(void);

#endif
