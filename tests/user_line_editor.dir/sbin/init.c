/*
 * Tests the shell's interactive line editor (root/shell/line_editor.c) by
 * feeding it the same bytes the terminal forwards for keystrokes and checking
 * the resulting command buffer, cursor position, and returned status:
 * - insertion and deletion in the middle of the line, cursor movement keys
 * - Backspace (0x08) and Delete (0x7F) deleting in opposite directions
 * - Ctrl+U / Ctrl+K / Ctrl+W kill commands and buffer-overflow truncation
 * - Enter / Ctrl+C results
 * - history recall with Up/Down, including restoring the unsent draft,
 *   skipping blank and duplicate lines, and not mutating entries when a
 *   recalled line is edited
 * - tab completion at the cursor, both at the end of the line and in the
 *   middle of it, using the docs/ tree packaged with this test
 *
 * The editor's screen echo also goes to stdout. It never starts a line with
 * "***", and each report below begins with '\n', so only the reports reach
 * the golden output.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/unistd.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/ps2.h"
#include "../../../root/shell/shell.h"
#include "../../../root/shell/line_editor.h"
#include "../../user_test.h"

// Feed each byte of text as a keystroke and return the last status.
static enum LineEditorStatus type(char* text){
  enum LineEditorStatus status = LINE_EDITOR_CONTINUE;
  while (*text != '\0'){
    status = line_editor_handle_key((unsigned char)*text);
    text++;
  }
  return status;
}

// Feed one keystroke `count` times.
static enum LineEditorStatus press(unsigned char key, unsigned count){
  enum LineEditorStatus status = LINE_EDITOR_CONTINUE;
  for (unsigned i = 0; i < count; i++){
    status = line_editor_handle_key(key);
  }
  return status;
}

// Byte the terminal forwards for Ctrl+<letter>.
static unsigned char ctrl(char letter){
  return (unsigned char)(letter - 'a' + 1);
}

// Report whether the edited line and cursor match the expectation.
static void expect_line(char* operation, char* expected, unsigned cursor){
  cmd_buf[cmd_buf_len] = '\0';
  int args[5] = {(int)operation, (int)cmd_buf, (int)line_editor_cursor,
                 (int)expected, (int)cursor};
  if (streq(cmd_buf, expected) && line_editor_cursor == cursor){
    printf("\n***PASS %s: line '%s' cursor %u\n", args);
  } else {
    printf("\n***FAIL %s: got line '%s' cursor %u, expected '%s' cursor %u\n",
           args);
  }
}

// Report an integer expectation on its own line, clear of editor echo.
static void expect_eq(char* operation, int actual, int expected){
  puts("\n");
  user_test_expect_eq(operation, actual, expected);
}

int main(void){
  user_test_expect_eq("chdir to test root", chdir("/"), 0);

  // Cursor movement and editing in the middle of the line.
  line_editor_begin(0);
  type("hllo");
  press(KEY_HOME, 1);
  press(KEY_RIGHT, 1);
  type("e");
  expect_line("insert after Home+Right", "hello", 2);

  press(KEY_END, 1);
  press(KEY_LEFT, 2);
  press(KEY_BACKSPACE, 1);
  expect_line("backspace mid-line", "helo", 2);

  press(KEY_DELETE, 1);
  expect_line("Delete removes the character at the cursor", "heo", 2);
  press(KEY_END, 1);
  press(KEY_DELETE, 1);
  expect_line("Delete at end of line is a no-op", "heo", 3);
  press(KEY_LEFT, 1);
  type("l");

  press(ctrl('a'), 1);
  press(ctrl('d'), 1);
  expect_line("Ctrl+D deletes at cursor", "elo", 0);

  press(KEY_LEFT, 1);
  expect_line("Left at start of line is a no-op", "elo", 0);
  press(ctrl('e'), 1);
  press(KEY_RIGHT, 1);
  expect_line("Right at end of line is a no-op", "elo", 3);

  // Kill commands.
  line_editor_begin(0);
  type("foo bar");
  press(ctrl('b'), 3);
  press(ctrl('u'), 1);
  expect_line("Ctrl+U deletes to start", "bar", 0);

  line_editor_begin(0);
  type("foo bar");
  press(KEY_HOME, 1);
  press(ctrl('f'), 3);
  press(ctrl('k'), 1);
  expect_line("Ctrl+K deletes to end", "foo", 3);

  line_editor_begin(0);
  type("ls docs  ");
  press(ctrl('w'), 1);
  expect_line("Ctrl+W deletes word and trailing spaces", "ls ", 3);

  // One byte of cmd_buf is reserved for the NUL terminator.
  line_editor_begin(0);
  for (unsigned i = 0; i < SHELL_COMMAND_BUFFER_SIZE + 10; i++){
    line_editor_handle_key('x');
  }
  expect_eq("overlong input stops at buffer capacity", (int)cmd_buf_len,
            SHELL_COMMAND_BUFFER_SIZE - 1);

  // Submit and cancel.
  line_editor_history_clear();
  line_editor_begin(0);
  type("echo hi");
  press(KEY_HOME, 1);
  expect_eq("Enter mid-line submits", (int)type("\n"), LINE_EDITOR_SUBMIT);
  expect_line("Enter keeps the whole line", "echo hi", 0);

  line_editor_begin(0);
  type("abc");
  expect_eq("Ctrl+C cancels", (int)press(ctrl('c'), 1), LINE_EDITOR_CANCEL);
  expect_line("Ctrl+C clears the line", "", 0);

  // History. "echo hi" is already recorded; blanks and repeats are skipped.
  line_editor_begin(0);
  type("second\n");
  line_editor_begin(0);
  type("second\n");
  line_editor_begin(0);
  type("   \n");

  line_editor_begin(0);
  type("draft");
  press(KEY_UP, 1);
  expect_line("Up recalls newest entry", "second", 6);
  press(KEY_UP, 1);
  expect_line("Up again recalls older entry", "echo hi", 7);
  press(ctrl('p'), 1);
  expect_line("Up past oldest entry stays put", "echo hi", 7);
  press(KEY_DOWN, 1);
  press(KEY_BACKSPACE, 1);
  expect_line("recalled entry can be edited", "secon", 5);
  press(ctrl('n'), 1);
  expect_line("Down past newest restores draft", "draft", 5);
  press(KEY_DOWN, 1);
  expect_line("Down on the draft stays put", "draft", 5);
  press(KEY_UP, 1);
  expect_line("editing a recalled line leaves history unchanged", "second",
              6);

  // Tab completion at the end of the line.
  line_editor_begin(0);
  type("cat docs/b\t");
  expect_line("single directory match appends '/'", "cat docs/beta/", 14);
  type("\t");
  expect_line("'.' and '..' are not offered", "cat docs/beta/keep ", 19);

  line_editor_begin(0);
  type("cat docs/al\t");
  expect_line("ambiguous matches insert shared prefix", "cat docs/alpha", 14);
  type("\t");
  expect_line("listing candidates leaves line unchanged", "cat docs/alpha",
              14);

  line_editor_begin(0);
  type("mkd\t");
  expect_line("first word completes built-in commands", "mkdir ", 6);

  line_editor_begin(0);
  type("ls mkd\t");
  expect_line("later words do not complete commands", "ls mkd", 6);

  // Tab completion in the middle of the line completes the word ending at
  // the cursor and leaves the text after it alone.
  line_editor_begin(0);
  type("cat docs/b x");
  press(KEY_LEFT, 2);
  type("\t");
  expect_line("mid-line completion inserts at cursor", "cat docs/beta/ x", 14);

  line_editor_begin(0);
  type("cat docs/alpha.t x");
  press(KEY_LEFT, 2);
  type("\t");
  expect_line("mid-line completion steps over existing space",
              "cat docs/alpha.txt x", 19);

  // Editing a line that wraps across terminal rows. The buffer checks run
  // here; the echo between the markers was also replayed through a host model
  // of the terminal's cursor rules to confirm the screen matches the buffer.
  puts("\n@@wrap-begin@@\n");
  line_editor_begin(print_line_prefix());
  for (unsigned i = 0; i < 15; i++){
    type("0123456789");
  }
  press(KEY_HOME, 1);
  type("X");
  press(KEY_END, 1);
  press(KEY_LEFT, 80);
  type("Y");
  press(ctrl('e'), 1);
  press(KEY_BACKSPACE, 75);
  press(KEY_LEFT, 3);
  press(ctrl('k'), 1);
  expect_eq("wrapped-line edits keep the expected length", (int)cmd_buf_len,
            74);
  type("\n");
  puts("@@wrap-end@@\n");

  line_editor_history_clear();
  printf("\n***Done.\n", NULL);
  return 0;
}
