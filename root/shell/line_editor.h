#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#include "../crt/stdbool.h"

// Number of submitted command lines remembered for Up/Down recall. The oldest
// entry is discarded once the history is full.
#define LINE_EDITOR_HISTORY_CAPACITY 64

// Result of feeding one input byte to the line editor.
enum LineEditorStatus {
  LINE_EDITOR_CONTINUE, // keep reading keys for the current line
  LINE_EDITOR_SUBMIT,   // cmd_buf holds a complete line ready to dispatch
  LINE_EDITOR_CANCEL    // the line was discarded (^C); show a fresh prompt
};

// Cursor position within cmd_buf, in [0, cmd_buf_len]. Exposed for tests.
extern unsigned line_editor_cursor;

// Start editing an empty line. prompt_width is the number of visible columns
// the prompt occupies; the prompt must already be printed starting at column 0
// of the terminal so the editor can compute where wrapped rows begin.
void line_editor_begin(unsigned prompt_width);

// Apply one byte from the terminal input stream (printable text, a control
// byte, or one of the single-byte KEY_* codes from ps2.h) to cmd_buf and echo
// the corresponding screen update to STDOUT.
enum LineEditorStatus line_editor_handle_key(unsigned char key);

// Append a submitted line to the history. Blank lines and an exact repeat of
// the most recent entry are not recorded.
void line_editor_history_add(char* line, unsigned length);

// Forget every history entry and release its storage.
void line_editor_history_clear(void);

#endif // LINE_EDITOR_H
