#ifndef SHELL_H
#define SHELL_H

// Shared command-buffer contract used by the interactive shell and focused
// command-dispatch tests. One byte always remains reserved for the trailing NUL.
#define SHELL_COMMAND_BUFFER_SIZE 2048

extern char cmd_buf[SHELL_COMMAND_BUFFER_SIZE];
extern unsigned cmd_buf_len;

void handle_command(void);

#endif // SHELL_H
