/* Emberfall input: raw PS/2 words or cooked bytes plus mouse events.
 *
 * Raw mode (set_keyboard_raw) delivers 16-bit little-endian words on stdin,
 * including releases with bit 8 set. Cooked fallback uses translated bytes
 * with a starfall-style latch so host key repeat steers smoothly. Mouse
 * motion accumulates into 640x480 screen pixels for menu clicks.
 */

#ifndef EMBER_INPUT_H
#define EMBER_INPUT_H

#include "emberfall.h"

/* Held-key table covers all 256 guest keycodes. */
struct InputState {
  int mouse_x;
  int mouse_y;
  int mouse_buttons;
  int prev_buttons;
  int raw_mode;
  int quit;
  int mute_toggle;
  int held[256];
  int latch_timer;
};

/* Drain stale mouse, enable raw keys when possible, center the cursor. */
void input_init(struct InputState* st);

/* Poll stdin and mouse into abstract game input plus quit/mute flags. */
void input_poll(struct InputState* st, int phase, struct GameInput* out);

/* True when the left button was pressed since the previous poll. */
int input_clicked(struct InputState* st);

#endif
