/* Translate terminal bytes and mouse words into game input. */

#include "input.h"
#include "render.h"

#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/mouse.h"
#include "../crt/ps2.h"

#define RAW_BUF_BYTES 64
#define LATCH_TICKS 11
#define MAX_MOUSE_EVENTS 32

/* Center the cursor, drain stale events, try raw keys. */
void input_init(struct InputState* st) {
  int i = 0;
  while (i < 256) {
    st->held[i] = 0;
    i++;
  }
  st->mouse_x = 320;
  st->mouse_y = 240;
  st->mouse_buttons = 0;
  st->prev_buttons = 0;
  st->latch_timer = 0;
  st->quit = 0;
  st->mute_toggle = 0;
  /* Drain stale motion so the cursor does not jump on entry. */
  {
    int n = 0;
    while (n < 64 && getmouse() != 0) n++;
  }
  /* Raw mode gives press and release words; fall back to cooked bytes.
   * set_keyboard_raw() returns once the terminal has switched, having
   * already discarded the cooked bytes queued before that. */
  if (set_keyboard_raw(1) == 0) st->raw_mode = 1;
  else st->raw_mode = 0;
}

/* Left-button press edge since the previous poll. */
int input_clicked(struct InputState* st) {
  return (st->mouse_buttons & MOUSE_BUTTON_LEFT) &&
    !(st->prev_buttons & MOUSE_BUTTON_LEFT);
}

/* Record one raw press for menu keys; releases only clear held state. */
static void raw_press(struct InputState* st, int phase, struct GameInput* out,
    int code) {
  if (code == 0x0D || code == 0x0A) {
    if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER ||
        phase == PHASE_PAUSE || phase == PHASE_HELP) {
      out->confirm = 1;
    }
    if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER) out->start = 1;
    return;
  }
  if (code == 0x1B) {
    if (phase == PHASE_PLAY) out->pause = 1;
    else if (phase == PHASE_PAUSE || phase == PHASE_HELP) out->pause = 1;
    else if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER) st->quit = 1;
    return;
  }
  if (code == 'p' || code == 'P') {
    if (phase == PHASE_PLAY || phase == PHASE_PAUSE) out->pause = 1;
    return;
  }
  if (code == 'm' || code == 'M') {
    st->mute_toggle = 1;
    return;
  }
  if (code == 'h' || code == 'H') {
    if (phase == PHASE_TITLE) out->pause = 1;
    return;
  }
  if (code == 'q' || code == 'Q') {
    st->quit = 1;
    return;
  }
  if (code == 'r' || code == 'R') {
    if (phase == PHASE_PAUSE || phase == PHASE_GAMEOVER) out->start = 1;
    return;
  }
  if (phase == PHASE_LEVELUP) {
    if (code == '1') out->choose = 0;
    else if (code == '2') out->choose = 1;
    else if (code == '3') out->choose = 2;
  }
}

/* Cooked press uses translated bytes (shifted letters, newline enter). */
static void cooked_press(struct InputState* st, int phase,
    struct GameInput* out, int code) {
  if (code == '\n') {
    if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER ||
        phase == PHASE_PAUSE || phase == PHASE_HELP) {
      out->confirm = 1;
    }
    if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER) out->start = 1;
    return;
  }
  if (code == 0x1B) {
    if (phase == PHASE_PLAY) out->pause = 1;
    else if (phase == PHASE_PAUSE || phase == PHASE_HELP) out->pause = 1;
    else if (phase == PHASE_TITLE || phase == PHASE_GAMEOVER) st->quit = 1;
    return;
  }
  if (code == 'p' || code == 'P') {
    if (phase == PHASE_PLAY || phase == PHASE_PAUSE) out->pause = 1;
    return;
  }
  if (code == 'm' || code == 'M') {
    st->mute_toggle = 1;
    return;
  }
  if (code == 'h' || code == 'H') {
    if (phase == PHASE_TITLE) out->pause = 1;
    return;
  }
  if (code == 'q' || code == 'Q') {
    st->quit = 1;
    return;
  }
  if (code == 'r' || code == 'R') {
    if (phase == PHASE_PAUSE || phase == PHASE_GAMEOVER) out->start = 1;
    return;
  }
  if (phase == PHASE_LEVELUP) {
    if (code == '1') out->choose = 0;
    else if (code == '2') out->choose = 1;
    else if (code == '3') out->choose = 2;
  }
}

/* Drain raw 16-bit words, updating held keys and menu presses. */
static void poll_raw(struct InputState* st, int phase, struct GameInput* out) {
  int avail = fd_bytes_available(STDIN);
  if (avail > 0) {
    char buf[64];
    int want = avail;
    int n = 0;
    int i = 0;
    if (want > 64) want = 64;
    want = (want / 2) * 2;
    if (want > 0) {
      n = read(STDIN, buf, want);
      while (i + 1 < n) {
        int lo = buf[i] & 0xFF;
        int hi = buf[i + 1] & 0xFF;
        int release = hi & 1;
        if (release) {
          st->held[lo] = 0;
        } else {
          st->held[lo] = 1;
          raw_press(st, phase, out, lo);
        }
        i += 2;
      }
    }
  }
}

/* Drain cooked bytes, refreshing the hold latch on every press. */
static void poll_cooked(struct InputState* st, int phase,
    struct GameInput* out) {
  int avail = fd_bytes_available(STDIN);
  if (avail > 0) {
    char buf[64];
    int want = avail;
    int n = 0;
    int i = 0;
    if (want > 64) want = 64;
    n = read(STDIN, buf, want);
    i = 0;
    while (i < n) {
      int code = buf[i] & 0xFF;
      st->held[code] = 1;
      st->latch_timer = LATCH_TICKS;
      cooked_press(st, phase, out, code);
      i++;
    }
  }
  if (st->latch_timer > 0) {
    st->latch_timer--;
    if (st->latch_timer == 0) {
      int i = 0;
      while (i < 256) {
        st->held[i] = 0;
        i++;
      }
    }
  }
}

/* Drain mouse events into an absolute screen position and buttons. */
static void poll_mouse(struct InputState* st) {
  int n = 0;
  st->prev_buttons = st->mouse_buttons;
  while (n < MAX_MOUSE_EVENTS) {
    int ev = getmouse();
    if (ev == 0) break;
    st->mouse_x += MOUSE_DX(ev);
    st->mouse_y += MOUSE_DY(ev);
    if (st->mouse_x < 0) st->mouse_x = 0;
    if (st->mouse_x > 639) st->mouse_x = 639;
    if (st->mouse_y < 0) st->mouse_y = 0;
    if (st->mouse_y > 479) st->mouse_y = 479;
    st->mouse_buttons = MOUSE_BUTTONS(ev);
    n++;
  }
}

/* Whether a screen point sits inside a tile-cell rectangle. */
static int inside_rect(int px, int py, int tx, int ty, int tw, int th) {
  int x0 = tx * 8;
  int y0 = ty * 8;
  int x1 = x0 + tw * 8;
  int y1 = y0 + th * 8;
  return px >= x0 && px < x1 && py >= y0 && py < y1;
}

/* Translate stdin and mouse into abstract game input. */
void input_poll(struct InputState* st, int phase, struct GameInput* out) {
  out->move_x = 0;
  out->move_y = 0;
  out->start = 0;
  out->pause = 0;
  out->choose = -1;
  out->confirm = 0;
  st->quit = 0;
  st->mute_toggle = 0;
  if (st->raw_mode) poll_raw(st, phase, out);
  else poll_cooked(st, phase, out);
  poll_mouse(st);
  /* Held movement from WASD and arrows in either mode. */
  {
    int left = st->held['a'] || st->held['A'] || st->held[134];
    int right = st->held['d'] || st->held['D'] || st->held[133];
    int up = st->held['w'] || st->held['W'] || st->held[136];
    int down = st->held['s'] || st->held['S'] || st->held[135];
    if (left && !right) out->move_x = -1;
    else if (right && !left) out->move_x = 1;
    if (up && !down) out->move_y = -1;
    else if (down && !up) out->move_y = 1;
  }
  /* Mouse clicks activate menu targets. */
  if (input_clicked(st)) {
    if (phase == PHASE_TITLE) {
      if (inside_rect(st->mouse_x, st->mouse_y, 28, 44, 24, 3)) {
        out->start = 1;
        out->confirm = 1;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 28, 48, 24, 3)) {
        out->pause = 1;
      }
    } else if (phase == PHASE_LEVELUP) {
      if (inside_rect(st->mouse_x, st->mouse_y, 8, 22, 20, 16)) {
        out->choose = 0;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 30, 22, 20, 16)) {
        out->choose = 1;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 52, 22, 20, 16)) {
        out->choose = 2;
      }
    } else if (phase == PHASE_PAUSE) {
      if (inside_rect(st->mouse_x, st->mouse_y, 30, 30, 20, 3)) {
        out->pause = 1;
        out->confirm = 1;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 30, 34, 20, 3)) {
        out->start = 1;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 30, 38, 20, 3)) {
        st->quit = 1;
      }
    } else if (phase == PHASE_GAMEOVER) {
      if (inside_rect(st->mouse_x, st->mouse_y, 28, 44, 24, 3)) {
        out->start = 1;
        out->confirm = 1;
      } else if (inside_rect(st->mouse_x, st->mouse_y, 28, 48, 24, 3)) {
        st->quit = 1;
      }
    } else if (phase == PHASE_HELP) {
      out->confirm = 1;
    }
  }
}
