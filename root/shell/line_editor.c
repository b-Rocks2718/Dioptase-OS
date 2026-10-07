#include "../crt/stdbool.h"
#include "../crt/stddef.h"
#include "../crt/string.h"
#include "../crt/stdlib.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"
#include "../crt/vga.h"

#include "dirs.h"
#include "shell.h"
#include "line_editor.h"

/*
 * Interactive line editor for the shell: cursor movement, in-place insertion
 * and deletion, history recall, and tab completion at the cursor.
 *
 * Screen model
 * ------------
 * The terminal (root/terminal/terminal.c) only offers relative cursor motion
 * that stays inside one row: '\b' and CSI C/D clamp at the row edges and CSI
 * A/B move whole rows. The editor therefore tracks where the terminal cursor
 * is as an offset from the prompt's first cell. Offset o is drawn at row
 * o / TILE_ROW_WIDTH and column o % TILE_ROW_WIDTH relative to that cell, and
 * cmd_buf[i] lives at offset prompt_width + i. Moving between two offsets is a
 * row move followed by a column move, which stays correct even after the
 * terminal scrolls because every move is relative.
 *
 * This relies on the prompt starting at column 0. If a previous program left
 * the cursor mid-row, lines that wrap are redrawn at the wrong place (lines
 * that fit on one row are unaffected).
 *
 * Wrapping: after a character is written to the last column of the bottom
 * row, the terminal defers the wrap until the next printable byte, and any
 * cursor-movement sequence cancels that deferral. Whenever drawing ends
 * exactly on a row boundary the editor writes " \b" to force the wrap, so the
 * terminal cursor always matches display_offset before the next move.
 */

// Ctrl+<letter> bytes forwarded by the terminal (letter - 'a' + 1).
#define KEY_CTRL_A 1
#define KEY_CTRL_B 2
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_E 5
#define KEY_CTRL_F 6
#define KEY_CTRL_K 11
#define KEY_CTRL_L 12
#define KEY_CTRL_N 14
#define KEY_CTRL_P 16
#define KEY_CTRL_U 21
#define KEY_CTRL_W 23

#define ASCII_FIRST_PRINTABLE ' '
#define ASCII_LAST_PRINTABLE '~'

// The terminal's CSI parser keeps at most two decimal digits per argument
// (MAX_ESCAPE_ARG_LENGTH in terminal.c), so longer moves are split.
#define TERMINAL_MAX_CSI_COUNT 99

// Echo bytes are batched so one keystroke costs one or two pipe writes.
#define OUTPUT_BUFFER_SIZE 256

unsigned line_editor_cursor = 0;

// Visible columns occupied by the prompt for the line being edited.
static unsigned prompt_width = 0;
// Terminal cursor position as an offset from the prompt's first cell.
static unsigned display_offset = 0;
// Buffer bytes currently drawn on screen. After a deletion this exceeds
// cmd_buf_len until the stale tail is cleared.
static unsigned displayed_length = 0;

// Submitted lines, oldest first. Each entry is a malloc'd NUL-terminated copy.
static char* history_entries[LINE_EDITOR_HISTORY_CAPACITY];
static unsigned history_count = 0;
// Entry currently shown while browsing; history_count means the user is on
// the new line rather than a recalled entry.
static unsigned history_position = 0;
// The in-progress line saved when browsing starts so Down can restore it.
static char* history_draft = NULL;

static char output_buffer[OUTPUT_BUFFER_SIZE];
static unsigned output_length = 0;

// Write every buffered echo byte to STDOUT. Partial writes are retried; a
// failed write drops the rest because there is nowhere to report it.
static void output_flush(void){
  unsigned written = 0;
  while (written < output_length){
    int result = write(STDOUT, output_buffer + written, output_length - written);
    if (result <= 0){
      break;
    }
    written += (unsigned)result;
  }
  output_length = 0;
}

// Queue one echo byte, flushing first if the buffer is full.
static void output_char(char c){
  if (output_length == OUTPUT_BUFFER_SIZE){
    output_flush();
  }
  output_buffer[output_length++] = c;
}

// Queue a NUL-terminated string of echo bytes.
static void output_string(char* str){
  while (*str != '\0'){
    output_char(*str++);
  }
}

// Emit CSI <count><command>, split into chunks the terminal can parse.
static void output_cursor_move(unsigned count, char command){
  while (count > 0){
    unsigned chunk = count > TERMINAL_MAX_CSI_COUNT ? TERMINAL_MAX_CSI_COUNT
                                                    : count;
    output_string("\x1b[");
    if (chunk >= 10){
      output_char((char)('0' + chunk / 10));
    }
    output_char((char)('0' + chunk % 10));
    output_char(command);
    count -= chunk;
  }
}

// Move the terminal cursor from display_offset to target.
static void move_to_offset(unsigned target){
  unsigned current_row = display_offset / TILE_ROW_WIDTH;
  unsigned target_row = target / TILE_ROW_WIDTH;
  unsigned current_col = display_offset % TILE_ROW_WIDTH;
  unsigned target_col = target % TILE_ROW_WIDTH;

  if (target_row < current_row){
    output_cursor_move(current_row - target_row, 'A');
  } else if (target_row > current_row){
    output_cursor_move(target_row - current_row, 'B');
  }

  if (target_col < current_col){
    output_cursor_move(current_col - target_col, 'D');
  } else if (target_col > current_col){
    output_cursor_move(target_col - current_col, 'C');
  }

  display_offset = target;
}

// If drawing stopped exactly at a row boundary, make the terminal perform the
// deferred wrap now (see the file comment) so the cursor is really at column 0
// of the next row.
static void resolve_pending_wrap(void){
  if (display_offset != 0 && display_offset % TILE_ROW_WIDTH == 0){
    output_string(" \b");
  }
}

// Reset the screen model after a prompt has just been printed.
static void start_display(unsigned new_prompt_width){
  prompt_width = new_prompt_width;
  display_offset = prompt_width;
  displayed_length = 0;
  resolve_pending_wrap();
}

// Redraw cmd_buf from index `from` to the end, erase whatever remains of a
// previously longer line, and leave the terminal cursor at line_editor_cursor.
static void redraw_from(unsigned from){
  move_to_offset(prompt_width + from);
  for (unsigned i = from; i < cmd_buf_len; i++){
    output_char(cmd_buf[i]);
  }
  display_offset = prompt_width + cmd_buf_len;
  resolve_pending_wrap();

  if (displayed_length > cmd_buf_len){
    // Erase only this row when the stale text ended on it; otherwise clear to
    // the end of the screen, which also covers the stale rows below.
    unsigned stale_last_row =
      (prompt_width + displayed_length - 1) / TILE_ROW_WIDTH;
    if (stale_last_row == display_offset / TILE_ROW_WIDTH){
      output_string("\x1b[K");
    } else {
      output_string("\x1b[J");
    }
  }
  displayed_length = cmd_buf_len;

  move_to_offset(prompt_width + line_editor_cursor);
}

// Print a fresh prompt below the current line and redraw the whole buffer,
// keeping the cursor position. Used after printing completion candidates.
static void reprint_prompt_and_line(void){
  output_flush();
  start_display(print_line_prefix());
  redraw_from(0);
}

// Insert bytes at the cursor, truncating if the command buffer would overflow
// (one byte stays reserved for the NUL).
static void insert_text(char* text, unsigned count){
  unsigned available = SHELL_COMMAND_BUFFER_SIZE - 1 - cmd_buf_len;
  if (count > available){
    count = available;
  }
  if (count == 0){
    return;
  }

  // Shift the tail right, copying backward because the ranges overlap.
  for (unsigned i = cmd_buf_len; i > line_editor_cursor; i--){
    cmd_buf[i - 1 + count] = cmd_buf[i - 1];
  }
  memcpy(cmd_buf + line_editor_cursor, text, count);
  cmd_buf_len += count;

  unsigned from = line_editor_cursor;
  line_editor_cursor += count;
  redraw_from(from);
}

// Remove cmd_buf[start, end) and leave the cursor at start.
static void delete_range(unsigned start, unsigned end){
  if (start >= end){
    return;
  }
  unsigned removed = end - start;
  for (unsigned i = end; i < cmd_buf_len; i++){
    cmd_buf[i - removed] = cmd_buf[i];
  }
  cmd_buf_len -= removed;
  line_editor_cursor = start;
  redraw_from(start);
}

// Move the logical and terminal cursor without changing the buffer.
static void set_cursor(unsigned position){
  line_editor_cursor = position;
  move_to_offset(prompt_width + position);
}

// Replace the whole line (for history recall) and put the cursor at its end.
static void replace_line(char* text){
  unsigned length = strlen(text);
  if (length > SHELL_COMMAND_BUFFER_SIZE - 1){
    length = SHELL_COMMAND_BUFFER_SIZE - 1;
  }
  memcpy(cmd_buf, text, length);
  cmd_buf_len = length;
  line_editor_cursor = length;
  redraw_from(0);
}

// Return a malloc'd NUL-terminated copy of cmd_buf.
static char* copy_command_line(void){
  char* copy = malloc(cmd_buf_len + 1);
  memcpy(copy, cmd_buf, cmd_buf_len);
  copy[cmd_buf_len] = '\0';
  return copy;
}

// Release the saved in-progress line, if any.
static void discard_history_draft(void){
  if (history_draft != NULL){
    free(history_draft);
    history_draft = NULL;
  }
}

// Show the next-older history entry (Up / Ctrl+P).
static void history_previous(void){
  if (history_position == 0){
    return;
  }
  if (history_position == history_count){
    // Leaving the new line: keep what was typed so Down can bring it back.
    discard_history_draft();
    history_draft = copy_command_line();
  }
  history_position--;
  replace_line(history_entries[history_position]);
}

// Show the next-newer history entry, or the saved draft past the newest one
// (Down / Ctrl+N).
static void history_next(void){
  if (history_position >= history_count){
    return;
  }
  history_position++;
  if (history_position == history_count){
    replace_line(history_draft != NULL ? history_draft : "");
    discard_history_draft();
  } else {
    replace_line(history_entries[history_position]);
  }
}

// See line_editor.h. Once full, the oldest entry is dropped to make room.
void line_editor_history_add(char* line, unsigned length){
  bool blank = true;
  for (unsigned i = 0; i < length; i++){
    if (line[i] != ' '){
      blank = false;
      break;
    }
  }
  if (blank){
    return;
  }

  if (history_count > 0){
    char* newest = history_entries[history_count - 1];
    if (strlen(newest) == length && strneq(newest, line, length)){
      return;
    }
  }

  if (history_count == LINE_EDITOR_HISTORY_CAPACITY){
    free(history_entries[0]);
    for (unsigned i = 1; i < history_count; i++){
      history_entries[i - 1] = history_entries[i];
    }
    history_count--;
  }

  char* entry = malloc(length + 1);
  memcpy(entry, line, length);
  entry[length] = '\0';
  history_entries[history_count++] = entry;
}

// See line_editor.h. Also abandons any in-progress history browsing.
void line_editor_history_clear(void){
  for (unsigned i = 0; i < history_count; i++){
    free(history_entries[i]);
  }
  history_count = 0;
  history_position = 0;
  discard_history_draft();
}

// True for the "." and ".." directory entries.
static bool is_dot_or_dot_dot(char* name){
  return name[0] == '.' &&
         (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// Free and unlink every "." and ".." entry from a completion list.
static struct LinkedDirent* remove_dot_entries(struct LinkedDirent* head){
  struct LinkedDirent* kept_head = NULL;
  struct LinkedDirent* kept_tail = NULL;
  while (head != NULL){
    struct LinkedDirent* next = head->next;
    if (is_dot_or_dot_dot(&head->dirent.d_name)){
      free(head);
    } else {
      head->next = NULL;
      if (kept_tail == NULL){
        kept_head = head;
      } else {
        kept_tail->next = head;
      }
      kept_tail = head;
    }
    head = next;
  }
  return kept_head;
}

// Length of the longest prefix shared by every name in a non-empty list.
static unsigned common_name_prefix_length(struct LinkedDirent* head){
  char* first = &head->dirent.d_name;
  unsigned length = strlen(first);
  for (struct LinkedDirent* current = head->next; current != NULL;
       current = current->next){
    char* name = &current->dirent.d_name;
    unsigned shared = 0;
    while (shared < length && name[shared] == first[shared]){
      shared++;
    }
    length = shared;
  }
  return length;
}

/*
 * Complete the word that ends at the cursor (text after the cursor is left
 * alone). The first word on the line also matches /sbin programs and shell
 * built-ins unless it contains a '/'.
 *
 * - One match: insert the rest of its name plus '/' for a directory or ' '
 *   otherwise (stepping over that character if it is already next).
 * - Several matches with a longer shared prefix: insert the shared part.
 * - Several matches and nothing to add: list them under the line and reprint
 *   the prompt.
 *
 * "." and ".." are only offered when the final path component starts with '.'.
 */
static void complete_at_cursor(void){
  unsigned word_start = line_editor_cursor;
  while (word_start > 0 && cmd_buf[word_start - 1] != ' '){
    word_start--;
  }

  bool is_command_word = true;
  for (unsigned i = 0; i < word_start; i++){
    if (cmd_buf[i] != ' '){
      is_command_word = false;
      break;
    }
  }

  // Start of the final path component, i.e. the part matched against names.
  unsigned component_start = word_start;
  for (unsigned i = word_start; i < line_editor_cursor; i++){
    if (cmd_buf[i] == '/'){
      component_start = i + 1;
      is_command_word = false;
    }
  }
  unsigned component_length = line_editor_cursor - component_start;
  bool hide_dot_entries = component_length == 0 ||
                          cmd_buf[component_start] != '.';

  unsigned word_length = line_editor_cursor - word_start;
  char* word = malloc(word_length + 1);
  memcpy(word, cmd_buf + word_start, word_length);
  word[word_length] = '\0';

  struct LinkedDirent* matches = tab_complete_directory(word, is_command_word);
  free(word);
  if (hide_dot_entries){
    matches = remove_dot_entries(matches);
  }
  if (matches == NULL){
    return;
  }

  unsigned match_count = 0;
  for (struct LinkedDirent* current = matches; current != NULL;
       current = current->next){
    match_count++;
  }

  // Every match begins with the component, so the shared prefix is at least
  // component_length long.
  unsigned shared_length = common_name_prefix_length(matches);
  unsigned new_characters = shared_length - component_length;
  if (new_characters > 0){
    insert_text(&matches->dirent.d_name + component_length, new_characters);
  }

  if (match_count == 1){
    char suffix = matches->d_type == DT_DIR ? '/' : ' ';
    if (line_editor_cursor < cmd_buf_len &&
        cmd_buf[line_editor_cursor] == suffix){
      set_cursor(line_editor_cursor + 1);
    } else {
      insert_text(&suffix, 1);
    }
  } else if (new_characters == 0){
    move_to_offset(prompt_width + cmd_buf_len);
    output_char('\n');
    output_flush();
    print_directory(matches, hide_dot_entries);
    reprint_prompt_and_line();
  }

  destroy_linked_dirents(matches);
}

// Delete the word before the cursor along with any spaces between it and the
// cursor, matching the usual Ctrl+W behavior.
static void delete_previous_word(void){
  unsigned start = line_editor_cursor;
  while (start > 0 && cmd_buf[start - 1] == ' '){
    start--;
  }
  while (start > 0 && cmd_buf[start - 1] != ' '){
    start--;
  }
  delete_range(start, line_editor_cursor);
}

// Ctrl+L: clear the screen and redraw the prompt and line at the top.
static void clear_screen_and_redraw(void){
  output_string("\x1b[2J\x1b[H");
  reprint_prompt_and_line();
}

// See line_editor.h. Also leaves history browsing so Up starts from the newest
// entry again.
void line_editor_begin(unsigned new_prompt_width){
  cmd_buf_len = 0;
  cmd_buf[0] = '\0';
  line_editor_cursor = 0;
  history_position = history_count;
  discard_history_draft();
  start_display(new_prompt_width);
  output_flush();
}

// See line_editor.h. Echo output is flushed before returning so the terminal
// is up to date whenever the shell blocks on the next read.
enum LineEditorStatus line_editor_handle_key(unsigned char key){
  enum LineEditorStatus status = LINE_EDITOR_CONTINUE;

  if (key == '\n' || key == '\r'){
    // Park the cursor after the line so command output starts below it.
    move_to_offset(prompt_width + cmd_buf_len);
    output_char('\n');
    cmd_buf[cmd_buf_len] = '\0';
    line_editor_history_add(cmd_buf, cmd_buf_len);
    status = LINE_EDITOR_SUBMIT;
  } else if (key == KEY_CTRL_C){
    move_to_offset(prompt_width + cmd_buf_len);
    output_string("^C\n");
    cmd_buf_len = 0;
    cmd_buf[0] = '\0';
    line_editor_cursor = 0;
    status = LINE_EDITOR_CANCEL;
  } else if (key == '\t'){
    complete_at_cursor();
  } else if (key == KEY_BACKSPACE){
    if (line_editor_cursor > 0){
      delete_range(line_editor_cursor - 1, line_editor_cursor);
    }
  } else if (key == KEY_DELETE || key == KEY_CTRL_D){
    if (line_editor_cursor < cmd_buf_len){
      delete_range(line_editor_cursor, line_editor_cursor + 1);
    }
  } else if (key == KEY_LEFT || key == KEY_CTRL_B){
    if (line_editor_cursor > 0){
      set_cursor(line_editor_cursor - 1);
    }
  } else if (key == KEY_RIGHT || key == KEY_CTRL_F){
    if (line_editor_cursor < cmd_buf_len){
      set_cursor(line_editor_cursor + 1);
    }
  } else if (key == KEY_HOME || key == KEY_CTRL_A){
    set_cursor(0);
  } else if (key == KEY_END || key == KEY_CTRL_E){
    set_cursor(cmd_buf_len);
  } else if (key == KEY_UP || key == KEY_CTRL_P){
    history_previous();
  } else if (key == KEY_DOWN || key == KEY_CTRL_N){
    history_next();
  } else if (key == KEY_CTRL_U){
    delete_range(0, line_editor_cursor);
  } else if (key == KEY_CTRL_K){
    delete_range(line_editor_cursor, cmd_buf_len);
  } else if (key == KEY_CTRL_W){
    delete_previous_word();
  } else if (key == KEY_CTRL_L){
    clear_screen_and_redraw();
  } else if (key >= ASCII_FIRST_PRINTABLE && key <= ASCII_LAST_PRINTABLE){
    char c = (char)key;
    insert_text(&c, 1);
  }
  // Anything else (ESC, Page Up/Down, other control bytes) is ignored.

  output_flush();
  return status;
}
