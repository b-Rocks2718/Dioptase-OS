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
// turns it off too.
#define TERMINAL_RAW_KEYBOARD_CSI_ARG 68
#define TERMINAL_RAW_KEYBOARD_ON "\x1b[?68h"
#define TERMINAL_RAW_KEYBOARD_OFF "\x1b[?68l"
#define TERMINAL_RAW_RECORD_BYTES 2
#define TERMINAL_RAW_RELEASE 1

#endif // TERMINAL_H
