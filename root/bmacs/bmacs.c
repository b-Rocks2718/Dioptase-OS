#include "../crt/sys.h"
#include "../crt/stdbool.h"
#include "../crt/stddef.h"
#include "../crt/ctype.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"
#include "../crt/vga.h"
#include "../crt/ps2.h"

#define MAX_FILE_READ_BYTES_PER_SYSCALL 1024
// bmacs uses one top border row plus a three-line footer, leaving 56 rows of
// editable text in the middle of the 80x60 tile display.
#define BMACS_TEXT_ROWS 56
#define BMACS_TEXT_COLS 80
#define BMACS_STATUS_TEXT_BYTES 81
#define BMACS_TEXT_TOP_ROW 1
#define BMACS_BORDER_TOP_ROW 0
#define BMACS_FOOTER_TOP_ROW 57
#define BMACS_STATUS_ROW 58
#define BMACS_BORDER_BOTTOM_ROW 59
#define TAB_SPACE_COUNT 2
#define BMACS_BORDER_COLOR 0x92
#define BMACS_TEXT_COLOR 0xFF
#define BMACS_CURSOR_COLOR 0xFF
#define ASCII_ETX 0x03
#define ASCII_DC3 0x13

struct TextBuffer {
  char* bytes;
  unsigned length;
  unsigned capacity;
};

struct EditorState {
  struct TextBuffer text;
  unsigned cursor_index;
  unsigned preferred_col;
  unsigned top_display_row;
  char status_text[BMACS_STATUS_TEXT_BYTES];
};

// Present one prepared 56x80 character viewport into the mapped tile
// framebuffer. The implementation lives in local assembly because the C
// compiler generates very slow code for the nested blit loops.
void bmacs_present_viewport_asm(short* tile_fb, char* render_rows);

bool shift_held = false;
bool ctrl_held = false;
bool alt_held = false;
static short* tile_fb = NULL;
static char render_rows[BMACS_TEXT_ROWS][BMACS_TEXT_COLS];
static unsigned cursor_fb_row = BMACS_TEXT_TOP_ROW;
static unsigned cursor_fb_col = 0;

static void init_text_buffer(struct TextBuffer* buffer){
  buffer->bytes = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

static int read_input_event(void){
  int available = fd_bytes_available(STDIN);

  if (available > 0){
    unsigned char byte;
    if (read(STDIN, &byte, 1) == 1){
      return byte;
    }
    return 0;
  }

  if (available < 0){
    return getkey();
  }

  return 0;
}

static void init_editor_state(struct EditorState* editor){
  init_text_buffer(&editor->text);
  editor->cursor_index = 0;
  editor->preferred_col = 0;
  editor->top_display_row = 0;
  memset(editor->status_text, ' ', BMACS_TEXT_COLS);
  editor->status_text[BMACS_TEXT_COLS] = '\0';
}

static void free_text_buffer(struct TextBuffer* buffer){
  free(buffer->bytes);
  init_text_buffer(buffer);
}

static void free_editor_state(struct EditorState* editor){
  free_text_buffer(&editor->text);
  editor->cursor_index = 0;
  editor->preferred_col = 0;
  editor->top_display_row = 0;
}

static void print_bmacs_file_error(char* operation, char* filename){
  puts("bmacs: ");
  puts(operation);
  puts(": ");
  puts(filename);
  puts("\n");
}

static void clear_status_text(struct EditorState* editor){
  memset(editor->status_text, ' ', BMACS_TEXT_COLS);
  editor->status_text[BMACS_TEXT_COLS] = '\0';
}

static void append_status_segment(
  struct EditorState* editor,
  unsigned* col,
  char* text
){
  while (*text != '\0' && *col < BMACS_TEXT_COLS){
    editor->status_text[*col] = *text;
    *col += 1;
    text += 1;
  }
}

static void set_status_message(
  struct EditorState* editor,
  char* prefix,
  char* suffix
){
  unsigned col = 0;

  clear_status_text(editor);
  append_status_segment(editor, &col, prefix);
  if (suffix != NULL){
    append_status_segment(editor, &col, suffix);
  }
  if (col != 0 && col < BMACS_TEXT_COLS){
    append_status_segment(editor, &col, " | ");
  }
  append_status_segment(editor, &col, "Ctrl-S save | Ctrl-C exit");
}

static void set_default_status(struct EditorState* editor){
  set_status_message(
    editor,
    "bmacs text editor",
    NULL
  );
}

static void hide_terminal_cursor(void){
  puts("\x1b[?25l");
}

static void show_terminal_cursor(void){
  puts("\x1b[?25h");
}

static short make_tile_entry(unsigned tile_index, unsigned color){
  return (short)(((color & 0xFFu) << 8) | (tile_index & 0xFFu));
}

static unsigned tile_fb_index(unsigned row, unsigned col){
  return row * TILE_ROW_WIDTH + col;
}

static void draw_editor_cursor(void){
  tile_fb[tile_fb_index(cursor_fb_row, cursor_fb_col)] =
    make_tile_entry(SQUARE_TILE, BMACS_CURSOR_COLOR);
}

static bool init_editor_display(void){
  load_text_tiles();
  set_tile_scale(0);
  set_vscroll(0);
  set_hscroll(0);

  tile_fb = get_tile_fb();
  if (tile_fb == NULL || (unsigned)tile_fb == 0xFFFFFFFFu){
    puts("bmacs: failed to map tile framebuffer\n");
    return false;
  }

  return true;
}

static void fill_render_rows(char c){
  memset(render_rows, c, BMACS_TEXT_ROWS * BMACS_TEXT_COLS);
}

static void advance_display_position(unsigned* row, unsigned* col, char c){
  unsigned spaces_remaining;

  if (c == '\n'){
    *row += 1;
    *col = 0;
    return;
  }

  if (c == '\t'){
    spaces_remaining = TAB_SPACE_COUNT;
    while (spaces_remaining != 0){
      *col += 1;
      if (*col == BMACS_TEXT_COLS){
        *row += 1;
        *col = 0;
      }
      spaces_remaining -= 1;
    }
    return;
  }

  *col += 1;
  if (*col == BMACS_TEXT_COLS){
    *row += 1;
    *col = 0;
  }
}

// Cursor indices point between bytes in the owned text buffer. This converts
// one logical insertion point into the wrapped viewport row/column coordinates.
static void buffer_position_for_index(
  struct TextBuffer* buffer,
  unsigned index,
  unsigned* row,
  unsigned* col
){
  *row = 0;
  *col = 0;

  if (index > buffer->length){
    index = buffer->length;
  }

  for (unsigned i = 0; i < index; ++i){
    advance_display_position(row, col, buffer->bytes[i]);
  }
}

static void update_preferred_col(struct EditorState* editor){
  unsigned row;
  unsigned col;

  buffer_position_for_index(
    &editor->text,
    editor->cursor_index,
    &row,
    &col
  );
  editor->preferred_col = col;
}

static bool ensure_text_buffer_capacity(
  struct TextBuffer* buffer,
  unsigned required_capacity
){
  char* grown_bytes;
  unsigned new_capacity;

  if (required_capacity <= buffer->capacity){
    return true;
  }

  new_capacity = buffer->capacity;
  if (new_capacity == 0){
    new_capacity = 1;
  }

  while (new_capacity < required_capacity){
    new_capacity *= 2;
  }

  grown_bytes = (char*) malloc(new_capacity);
  if (buffer->bytes != NULL){
    memcpy(grown_bytes, buffer->bytes, buffer->length + 1);
    free(buffer->bytes);
  }

  buffer->bytes = grown_bytes;
  buffer->capacity = new_capacity;
  return true;
}

static bool insert_text_char(
  struct TextBuffer* buffer,
  unsigned index,
  char c
){
  if (index > buffer->length){
    return false;
  }

  ensure_text_buffer_capacity(buffer, buffer->length + 2);

  for (unsigned i = buffer->length + 1; i > index; --i){
    buffer->bytes[i] = buffer->bytes[i - 1];
  }

  buffer->bytes[index] = c;
  buffer->length += 1;
  return true;
}

static bool delete_text_char_before(
  struct TextBuffer* buffer,
  unsigned* index
){
  if (*index == 0 || *index > buffer->length){
    return false;
  }

  for (unsigned i = *index - 1; i < buffer->length; ++i){
    buffer->bytes[i] = buffer->bytes[i + 1];
  }

  buffer->length -= 1;
  *index -= 1;
  return true;
}

static void expand_tabs_to_spaces(struct TextBuffer* buffer){
  unsigned tab_count = 0;
  char* expanded_bytes;
  unsigned expanded_length;
  unsigned src_index;
  unsigned dst_index;

  for (src_index = 0; src_index < buffer->length; ++src_index){
    if (buffer->bytes[src_index] == '\t'){
      tab_count += 1;
    }
  }

  if (tab_count == 0){
    return;
  }

  expanded_length = buffer->length + tab_count;
  expanded_bytes = (char*) malloc(expanded_length + 1);
  dst_index = 0;

  for (src_index = 0; src_index < buffer->length; ++src_index){
    if (buffer->bytes[src_index] == '\t'){
      expanded_bytes[dst_index] = ' ';
      dst_index += 1;
      expanded_bytes[dst_index] = ' ';
      dst_index += 1;
    } else {
      expanded_bytes[dst_index] = buffer->bytes[src_index];
      dst_index += 1;
    }
  }

  expanded_bytes[expanded_length] = '\0';
  free(buffer->bytes);
  buffer->bytes = expanded_bytes;
  buffer->length = expanded_length;
  buffer->capacity = expanded_length + 1;
}

static bool write_all_file_bytes(int fd, char* bytes, unsigned length){
  unsigned written = 0;

  while (written < length){
    unsigned chunk_size = length - written;
    int write_rc;

    if (chunk_size > MAX_FILE_READ_BYTES_PER_SYSCALL){
      chunk_size = MAX_FILE_READ_BYTES_PER_SYSCALL;
    }

    write_rc = write(fd, bytes + written, chunk_size);
    if (write_rc <= 0){
      return false;
    }

    written += (unsigned)write_rc;
  }

  return true;
}

static bool save_text_buffer_to_file(struct TextBuffer* buffer, char* filename){
  int fd = open(filename);
  bool ok = true;

  if (fd < 0){
    return false;
  }

  if (truncate(fd, 0) < 0){
    ok = false;
  } else if (seek(fd, 0, SEEK_SET) < 0){
    ok = false;
  } else if (!write_all_file_bytes(fd, buffer->bytes, buffer->length)){
    ok = false;
  }

  if (close(fd) < 0){
    ok = false;
  }

  return ok;
}

// Snapshot the current file contents into editor-owned heap storage so later
// editing does not depend on the kernel file descriptor staying open.
static bool load_text_buffer_from_file(
  struct TextBuffer* buffer,
  int fd,
  char* filename
){
  int file_size = seek(fd, 0, SEEK_END);
  if (file_size < 0){
    print_bmacs_file_error("failed to measure file", filename);
    return false;
  }

  if (seek(fd, 0, SEEK_SET) < 0){
    print_bmacs_file_error("failed to rewind file", filename);
    return false;
  }

  buffer->capacity = (unsigned)file_size + 1;
  buffer->bytes = (char*) malloc(buffer->capacity);

  while (buffer->length < (unsigned)file_size){
    unsigned remaining = (unsigned)file_size - buffer->length;
    unsigned chunk_size = remaining;
    int bytes_read;

    if (chunk_size > MAX_FILE_READ_BYTES_PER_SYSCALL){
      chunk_size = MAX_FILE_READ_BYTES_PER_SYSCALL;
    }

    bytes_read = read(fd, buffer->bytes + buffer->length, chunk_size);
    if (bytes_read < 0){
      print_bmacs_file_error("failed to read file", filename);
      free_text_buffer(buffer);
      return false;
    }
    if (bytes_read == 0){
      print_bmacs_file_error("hit unexpected EOF while reading file", filename);
      free_text_buffer(buffer);
      return false;
    }

    buffer->length += (unsigned)bytes_read;
  }

  buffer->bytes[buffer->length] = '\0';
  expand_tabs_to_spaces(buffer);
  return true;
}

// Vertical cursor movement works in wrapped screen rows, not only explicit
// newline-delimited file rows, so search the buffer's display layout directly.
static bool find_index_for_display_row_col(
  struct TextBuffer* buffer,
  unsigned target_row,
  unsigned target_col,
  unsigned* index_out
){
  unsigned row = 0;
  unsigned col = 0;
  unsigned last_index_on_target_row = 0;
  bool found_target_row = false;

  for (unsigned index = 0; ; ++index){
    if (row == target_row){
      found_target_row = true;
      last_index_on_target_row = index;
      if (col >= target_col){
        *index_out = index;
        return true;
      }
    } else if (found_target_row){
      *index_out = last_index_on_target_row;
      return true;
    }

    if (index == buffer->length){
      break;
    }

    advance_display_position(&row, &col, buffer->bytes[index]);
  }

  if (!found_target_row){
    return false;
  }

  *index_out = last_index_on_target_row;
  return true;
}

static void ensure_cursor_visible(struct EditorState* editor){
  unsigned cursor_row;
  unsigned cursor_col;

  buffer_position_for_index(
    &editor->text,
    editor->cursor_index,
    &cursor_row,
    &cursor_col
  );

  if (cursor_row < editor->top_display_row){
    editor->top_display_row = cursor_row;
  } else if (cursor_row >= editor->top_display_row + BMACS_TEXT_ROWS){
    editor->top_display_row = cursor_row - BMACS_TEXT_ROWS + 1;
  }
}

static void render_editor(struct EditorState* editor){
  unsigned row = 0;
  unsigned col = 0;
  unsigned cursor_row;
  unsigned cursor_col;
  unsigned status_col;

  ensure_cursor_visible(editor);

  fill_render_rows(' ');

  for (unsigned i = 0; i < editor->text.length; ++i){
    char c = editor->text.bytes[i];

    if (c == '\n'){
      advance_display_position(&row, &col, c);
      continue;
    }

    if (c == '\t'){
      unsigned tab_row = row;
      unsigned tab_col = col;
      unsigned spaces_remaining = TAB_SPACE_COUNT;

      while (spaces_remaining != 0){
        if (tab_row >= editor->top_display_row &&
            tab_row < editor->top_display_row + BMACS_TEXT_ROWS){
          render_rows[tab_row - editor->top_display_row][tab_col] = ' ';
        }

        tab_col += 1;
        if (tab_col == BMACS_TEXT_COLS){
          tab_row += 1;
          tab_col = 0;
        }
        spaces_remaining -= 1;
      }

      advance_display_position(&row, &col, c);
      continue;
    }

    if (row >= editor->top_display_row &&
        row < editor->top_display_row + BMACS_TEXT_ROWS){
      render_rows[row - editor->top_display_row][col] = c;
    }

    advance_display_position(&row, &col, c);
  }

  bmacs_present_viewport_asm(tile_fb, &render_rows[0][0]);
  for (status_col = 0; status_col < BMACS_TEXT_COLS; ++status_col){
    tile_fb[tile_fb_index(BMACS_STATUS_ROW, status_col)] =
      make_tile_entry((unsigned char)editor->status_text[status_col],
                      BMACS_TEXT_COLOR);
  }

  buffer_position_for_index(
    &editor->text,
    editor->cursor_index,
    &cursor_row,
    &cursor_col
  );
  cursor_fb_row = (cursor_row - editor->top_display_row) + BMACS_TEXT_TOP_ROW;
  cursor_fb_col = cursor_col;
  draw_editor_cursor();
}

static char apply_shift_to_char(char c){
  if (isalpha(c)){
    if (c >= 'a' && c <= 'z'){
      return c - 'a' + 'A';
    }
    return c;
  }

  if (c == '0'){
    return ')';
  } else if (c == '1'){
    return '!';
  } else if (c == '2'){
    return '@';
  } else if (c == '3'){
    return '#';
  } else if (c == '4'){
    return '$';
  } else if (c == '5'){
    return '%';
  } else if (c == '6'){
    return '^';
  } else if (c == '7'){
    return '&';
  } else if (c == '8'){
    return '*';
  } else if (c == '9'){
    return '(';
  } else if (c == '-'){
    return '_';
  } else if (c == '='){
    return '+';
  } else if (c == '['){
    return '{';
  } else if (c == ']'){
    return '}';
  } else if (c == '\\'){
    return '|';
  } else if (c == ';'){
    return ':';
  } else if (c == '\''){
    return '"';
  } else if (c == ','){
    return '<';
  } else if (c == '.'){
    return '>';
  } else if (c == '/'){
    return '?';
  } else if (c == '`'){
    return '~';
  }

  return c;
}

static void move_cursor_left(struct EditorState* editor){
  if (editor->cursor_index != 0){
    editor->cursor_index -= 1;
  }
  update_preferred_col(editor);
}

static void move_cursor_right(struct EditorState* editor){
  if (editor->cursor_index < editor->text.length){
    editor->cursor_index += 1;
  }
  update_preferred_col(editor);
}

static void move_cursor_up(struct EditorState* editor){
  unsigned row;
  unsigned col;
  unsigned target_index;

  buffer_position_for_index(
    &editor->text,
    editor->cursor_index,
    &row,
    &col
  );
  if (row == 0){
    return;
  }

  if (find_index_for_display_row_col(
        &editor->text,
        row - 1,
        editor->preferred_col,
        &target_index
      )){
    editor->cursor_index = target_index;
  }
}

static void move_cursor_down(struct EditorState* editor){
  unsigned row;
  unsigned col;
  unsigned target_index;

  buffer_position_for_index(
    &editor->text,
    editor->cursor_index,
    &row,
    &col
  );
  if (find_index_for_display_row_col(
        &editor->text,
        row + 1,
        editor->preferred_col,
        &target_index
      )){
    editor->cursor_index = target_index;
  }
}

// Backspace is a no-op at the start of the file. Everywhere else it removes the
// byte immediately before the cursor, including a newline that starts the
// current logical line.
static bool backspace_at_cursor(struct EditorState* editor){
  if (editor->cursor_index == 0){
    editor->preferred_col = 0;
    return false;
  }

  if (!delete_text_char_before(&editor->text, &editor->cursor_index)){
    return false;
  }

  update_preferred_col(editor);
  return true;
}

static void insert_spaces_at_cursor(struct EditorState* editor, unsigned count){
  while (count != 0){
    insert_text_char(&editor->text, editor->cursor_index, ' ');
    editor->cursor_index = editor->cursor_index + 1;
    count -= 1;
  }
  update_preferred_col(editor);
}

int main(int argc, char** argv){
  struct EditorState editor;
  bool running = true;
  bool needs_redraw = true;

  init_editor_state(&editor);
  set_default_status(&editor);

  if (argc < 2){
    puts("bmacs: expected file name as argument\n");
    return 1;
  }

  char* filename = argv[1];
  int fd = open(filename);
  if (fd < 0){
    print_bmacs_file_error("failed to open file", filename);
    return 1;
  }

  if (!load_text_buffer_from_file(&editor.text, fd, filename)){
    close(fd);
    return 1;
  }

  if (close(fd) < 0){
    print_bmacs_file_error("failed to close file", filename);
    free_editor_state(&editor);
    return 1;
  }

  hide_terminal_cursor();
  if (!init_editor_display()){
    show_terminal_cursor();
    free_editor_state(&editor);
    return 1;
  }

  while (running) {
    if (needs_redraw){
      render_editor(&editor);
      needs_redraw = false;
    }

    int c;
    while ((c = read_input_event()) != 0){
      if (c & 0xFF00) {
        c = c & 0xFF;
        if (c == KEY_LEFT_ALT || c == KEY_RIGHT_ALT) alt_held = false;
        if (c == KEY_LEFT_CTRL || c == KEY_RIGHT_CTRL) ctrl_held = false;
        if (c == KEY_LEFT_SHIFT || c == KEY_RIGHT_SHIFT) shift_held = false;
        continue;
      }
      if (c == '\n' || c == '\r'){
        insert_text_char(&editor.text, editor.cursor_index, '\n');
        editor.cursor_index = editor.cursor_index + 1;
        update_preferred_col(&editor);
        needs_redraw = true;
      } else if (c == '\b'){
        if (backspace_at_cursor(&editor)){
          needs_redraw = true;
        }
      } else if (c == '\t'){
        insert_spaces_at_cursor(&editor, TAB_SPACE_COUNT);
        needs_redraw = true;
      } else if (c == KEY_LEFT) {
        move_cursor_left(&editor);
        needs_redraw = true;
      } else if (c == KEY_RIGHT) {
        move_cursor_right(&editor);
        needs_redraw = true;
      } else if (c == KEY_UP) {
        move_cursor_up(&editor);
        needs_redraw = true;
      } else if (c == KEY_DOWN) {
        move_cursor_down(&editor);
        needs_redraw = true;
      } else if (c == ASCII_DC3) {
        if (save_text_buffer_to_file(&editor.text, filename)){
          set_status_message(&editor, "Saved ", filename);
        } else {
          set_status_message(&editor, "Save failed ", filename);
        }
        needs_redraw = true;
      } else if (c == ASCII_ETX) {
        running = false;
        break;
      } else if (isprint(c) || isspace(c)){
        if (ctrl_held){
          if (c == 's' || c == 'S'){
            if (save_text_buffer_to_file(&editor.text, filename)){
              set_status_message(&editor, "Saved ", filename);
            } else {
              set_status_message(&editor, "Save failed ", filename);
            }
            needs_redraw = true;
          } else if (c == 'c' || c == 'C'){
            running = false;
            break;
          }
        } else {
          char inserted = c;
          if (shift_held){
            inserted = apply_shift_to_char(inserted);
          }

          insert_text_char(&editor.text, editor.cursor_index, inserted);
          editor.cursor_index = editor.cursor_index + 1;
          update_preferred_col(&editor);
          needs_redraw = true;
        }
      } else if (c == KEY_LEFT_CTRL || c == KEY_RIGHT_CTRL){
        ctrl_held = true;
      } else if (c == KEY_LEFT_ALT || c == KEY_RIGHT_ALT){
        alt_held = true;
      } else if (c == KEY_LEFT_SHIFT || c == KEY_RIGHT_SHIFT){
        shift_held = true;
      } else {
        // Unsupported keys are ignored while bmacs owns the framebuffer.
      }
    }

    if (!running){
      break;
    }

    sleep(1);
  }

  show_terminal_cursor();
  free_editor_state(&editor);

  // clear the screen
  puts("\x1b[2J");

  // home cursor
  puts("\x1b[H");

  return 0;
}
