#ifndef TERMINAL_H
#define TERMINAL_H

// Dioptase-private CSI command used by the shell to request terminal-owned
// display recovery after a foreground program used direct VGA facilities.
// The numeric argument fits the terminal parser's two-digit argument limit.
#define TERMINAL_RESET_DISPLAY_CSI_ARG 67
#define TERMINAL_RESET_DISPLAY_SEQUENCE "\x1b[67J"

#endif // TERMINAL_H
