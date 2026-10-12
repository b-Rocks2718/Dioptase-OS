#ifndef TERMINAL_H
#define TERMINAL_H

// Dioptase-private CSI command used by the shell to request terminal-owned
// display recovery after a foreground program used direct VGA facilities.
// The numeric argument fits the terminal parser's two-digit argument limit.
#define TERMINAL_RESET_DISPLAY_CSI_ARG 67
#define TERMINAL_RESET_DISPLAY_SEQUENCE "\x1b[67J"

// Raw keyboard mode (Dioptase-private DEC-style mode 68, docs/terminal.md).
// While it is on, the terminal stops translating keys and instead forwards
// every keyboard event, presses and releases alike, as a two-byte record on
// the foreground program's STDIN:
//   byte 0: guest keycode (docs/mem_map.md, PS/2 keyboard input stream)
//   byte 1: TERMINAL_RAW_RELEASE for a release, 0 for a press
// Ctrl-C is not forwarded: it turns raw mode off and then terminates the
// foreground program as usual. The shell also turns the mode off whenever a
// foreground program exits, and display recovery (TERMINAL_RESET_DISPLAY_*)
// turns it off too. Programs normally switch with set_keyboard_raw()
// (root/crt/sys.h), which also waits for the switch to take effect.
#define TERMINAL_RAW_KEYBOARD_CSI_ARG 68
#define TERMINAL_RAW_KEYBOARD_ON "\x1b[?68h"
#define TERMINAL_RAW_KEYBOARD_OFF "\x1b[?68l"
#define TERMINAL_RAW_RECORD_BYTES 2
#define TERMINAL_RAW_RELEASE 1

// Keyboard sync request. The terminal answers CSI 68 n by writing
// TERMINAL_KEYBOARD_SYNC_BYTES bytes of TERMINAL_KEYBOARD_SYNC_REPLY_BYTE to
// the input pipe, in order with the key input it forwards, so everything a
// reader finds after the reply was forwarded after the terminal processed the
// requests written before the query. 0xFF is neither a guest keycode nor a
// cooked input byte, and a raw record's second byte is 0 or 1, so the pair
// cannot occur in either input form or straddle two records.
#define TERMINAL_KEYBOARD_SYNC_CSI_ARG 68
#define TERMINAL_KEYBOARD_SYNC "\x1b[68n"
#define TERMINAL_KEYBOARD_SYNC_BYTES 2
#define TERMINAL_KEYBOARD_SYNC_REPLY_BYTE 0xFF

#endif // TERMINAL_H
