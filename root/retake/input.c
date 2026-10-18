#include "../crt/sys.h"
#include "../crt/terminal.h"
#include "retake.h"

/*
 * Keyboard input. The terminal forwards raw key records on STDIN once
 * set_keyboard_raw(1) has taken effect (root/crt/terminal.h): byte 0 is the
 * guest keycode, byte 1 is TERMINAL_RAW_RELEASE for a release or 0 for a
 * press. A read may end between the two bytes, so the first waits in
 * pending_code.
 *
 * Controls: arrows or WASD move; Z, Space or K jumps; X or J claps (attack);
 * C or L records a take; V or ; jump-cuts; Esc pauses.
 */

static int key_down[256];
static int key_hits[256];      /* presses since the game last asked */
static int pending_code = -1;
static int pressed_bits;       /* input bits pressed since the last tick */

/* Input bits a key contributes, or 0. */
static int bits_for_key(int code) {
  switch (code) {
    case KEY_LEFT: case 'a': return IN_LEFT;
    case KEY_RIGHT: case 'd': return IN_RIGHT;
    case KEY_UP: case 'w': return IN_UP;
    case KEY_DOWN: case 's': return IN_DOWN;
    case 'z': case KEY_SPACE: case 'k': return IN_JUMP;
    case 'x': case 'j': return IN_SLASH;
    case 'c': case 'l': return IN_RECORD;
    case 'v': case ';': return IN_CUT;
    default: return 0;
  }
}

void input_init(void) {
  set_keyboard_raw(1);
}

/* Back to cooked input before the shell reads STDIN again. */
void input_shutdown(void) {
  set_keyboard_raw(0);
}

void input_poll(void) {
  unsigned char buf[32];
  int avail = fd_bytes_available(STDIN);
  while (avail > 0) {
    int n = read(STDIN, buf, avail < 32 ? avail : 32);
    if (n <= 0) break;
    avail -= n;
    for (int i = 0; i < n; i++) {
      if (pending_code < 0) {
        pending_code = buf[i];
        continue;
      }
      int code = pending_code;
      pending_code = -1;
      if (buf[i] == TERMINAL_RAW_RELEASE) {
        key_down[code] = 0;
      } else {
        if (!key_down[code]) {
          key_hits[code]++;
          pressed_bits |= bits_for_key(code);
        }
        key_down[code] = 1;
      }
    }
  }
}

/* Input bits for keys currently held. */
int input_held(void) {
  int bits = 0;
  for (int code = 0; code < 256; code++) {
    if (key_down[code]) bits |= bits_for_key(code);
  }
  return bits;
}

/* Input bits pressed since the last call; the controller consumes them on
 * the first tick of a frame so short taps between frames are never lost. */
int input_take_pressed(void) {
  int bits = pressed_bits;
  pressed_bits = 0;
  return bits;
}

int input_key_down(int code) {
  return key_down[code & 255];
}

/* Whether `code` was pressed since the last check (consumes the press). */
int input_key_hit(int code) {
  code &= 255;
  if (key_hits[code] == 0) return 0;
  key_hits[code] = 0;
  return 1;
}
