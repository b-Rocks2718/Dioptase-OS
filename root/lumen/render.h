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
#define SCREEN_BRIEFING 4

/* Renderer-owned RAM images use documented VGA 0x0BGR without transparency.
 * The view is redrawn each frame; HUD pixels are retained until inputs change.
 * External HUD writes require render_invalidate_hud() before the next frame.
 * Literal extents are VIEW_W*VIEW_H and HUD_W*HUD_H; bcc requires literals. */
extern unsigned short view_pixels[15360];
extern unsigned short hud_pixels[10240];
/* Changes once per HUD redraw, including an explicitly invalidated redraw. */
extern unsigned render_hud_revision;

/* Prepare procedural textures once; no assets or display mappings are required. */
void render_init(void);
/* Request a full HUD redraw after another owner has overwritten its RAM image. */
void render_invalidate_hud(void);
/* Render the world and UI with Q8 camera coordinates and a 256-step full turn. */
void render_frame(struct Game* game, int camera_x, int camera_y,
                  int angle, unsigned frame, int screen);
/* Validate raster clipping and deterministic rendering without device access. */
int render_selftest(void);

#endif
