/* Emberfall renderer: pixel playfield, tile HUD and menus, cursor sprite.
 *
 * The pixel layer holds the arena (320x236). The tile layer stays transparent
 * over the arena and carries HUD text plus menu boxes. Sprite 0 is the mouse
 * cursor in menus. All MMIO follows docs/mem_map.md through the CRT getters.
 */

#ifndef EMBER_RENDER_H
#define EMBER_RENDER_H

#include "emberfall.h"

struct HighScores {
  int score[5];
  int time_sec[5];
  int kills[5];
  int level[5];
};

/* Map display windows, set scales, load tiles, build art and background. */
void render_init(void);

/* Draw one gameplay frame: arena entities plus HUD. */
void render_frame(struct Game* game, int muted);

/* Menus fill pixels dark and draw tile text. */
void render_title(struct Game* game, struct HighScores* hs);
void render_help(void);
void render_pause(void);
void render_levelup(struct Game* game);
void render_gameover(struct Game* game, struct HighScores* hs,
  int new_best);

/* Cursor sprite follows the mouse in menus, hides during play. */
void render_cursor(int x, int y);
void render_hide_cursor(void);

/* Tile helpers shared with input hitboxes (cells, 80x60). */
void render_clear_tiles(void);
void render_text(int x, int y, char* text, int color);
void render_number(int x, int y, int value, int width, int color);

#endif
