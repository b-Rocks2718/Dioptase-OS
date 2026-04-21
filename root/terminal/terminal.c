#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"

#define TILE_ROW_WIDTH 80
#define TILE_COL_HEIGHT 60

#define TILE_HEIGHT 8

#define FB_NUM_TILES 4800
#define SQUARE_TILE 0x7F

#define CURSOR_BLINK_INTERVAL 10
#define MAIN_LOOP_DELAY_INTERVAL 1

#define TAB_WIDTH 8

#define MAX_ESCAPE_SEQUENCE_LENGTH 16
#define MAX_ESCAPE_ARG_LENGTH 2
#define MAX_ESCAPE_ARGS 2

#define TERMINAL_BUF_SIZE 128

short* TILE_FB = NULL;

// Cursor movement is relative to visible screen coordinates. Once the terminal
// starts scrolling, visible row 0 no longer matches physical TILE_FB row 0, so
// we map through scroll_top_row before touching the framebuffer.
int cursor_row = 0;
int cursor_col = 0;
int saved_cursor_row = 0;
int saved_cursor_col = 0;
bool saved_wrap_pending = false;
int scroll_top_row = 0;

bool cursor_visible = true;
bool cursor_drawn = false;
bool cursor_blink_on = true;
// next char printed should wrap to next line
bool wrap_pending = false;

unsigned last_cursor_blink = 0;

char current_color = 0xFF;
short under_cursor = 0x0000;
char terminal_buf[TERMINAL_BUF_SIZE];

static void put_terminal_char(char c);

static int min_int(int a, int b){
  return (a < b) ? a : b;
}

static int clamp_int(int value, int min_value, int max_value){
  if (value < min_value){
    return min_value;
  }
  if (value > max_value){
    return max_value;
  }
  return value;
}

static int visible_row_to_physical_row(int row){
  return (scroll_top_row + row) % TILE_COL_HEIGHT;
}

static int fb_index_for_position(int row, int col){
  return visible_row_to_physical_row(row) * TILE_ROW_WIDTH + col;
}

static int cursor_fb_index(void){
  return fb_index_for_position(cursor_row, cursor_col);
}

static void erase_cursor(void){
  if (!cursor_drawn){
    return;
  }

  TILE_FB[cursor_fb_index()] = under_cursor;
  cursor_drawn = false;
}

static void draw_cursor(void){
  int index;

  if (!cursor_visible || cursor_drawn){
    return;
  }

  index = cursor_fb_index();
  under_cursor = TILE_FB[index];
  TILE_FB[index] = 0xFF00 | SQUARE_TILE;
  cursor_drawn = true;
}

static void show_cursor_now(void){
  if (!cursor_visible){
    erase_cursor();
    return;
  }

  cursor_blink_on = true;
  last_cursor_blink = get_current_jiffies();
  draw_cursor();
}

static void blink_cursor(void){
  unsigned now = get_current_jiffies();

  if (!cursor_visible){
    erase_cursor();
    return;
  }

  if (now - last_cursor_blink < CURSOR_BLINK_INTERVAL){
    return;
  }

  last_cursor_blink = now;
  cursor_blink_on = !cursor_blink_on;
  if (cursor_blink_on){
    draw_cursor();
  } else {
    erase_cursor();
  }
}

static void clear_visible_row(int row){
  int base = fb_index_for_position(row, 0);
  for (int i = 0; i < TILE_ROW_WIDTH; ++i){
    TILE_FB[base + i] = 0;
  }
}

static void copy_visible_row(int dst_row, int src_row){
  int dst_base = fb_index_for_position(dst_row, 0);
  int src_base = fb_index_for_position(src_row, 0);

  for (int i = 0; i < TILE_ROW_WIDTH; ++i){
    TILE_FB[dst_base + i] = TILE_FB[src_base + i];
  }
}

static void scroll_terminal_one_line(void){
  scroll_top_row = (scroll_top_row + 1) % TILE_COL_HEIGHT;
  move_vscroll(-TILE_HEIGHT);
  clear_visible_row(TILE_COL_HEIGHT - 1);
  cursor_row = TILE_COL_HEIGHT - 1;
  cursor_col = 0;
}

static void move_cursor_up(int rows){
  cursor_row -= min_int(cursor_row, rows);
}

static void move_cursor_down(int rows){
  cursor_row += min_int(TILE_COL_HEIGHT - 1 - cursor_row, rows);
}

static void move_cursor_forward(int cols){
  cursor_col += min_int(TILE_ROW_WIDTH - 1 - cursor_col, cols);
}

static void move_cursor_backward(int cols){
  cursor_col -= min_int(cursor_col, cols);
}

static void cursor_home(void){
  cursor_row = 0;
  cursor_col = 0;
}

static void advance_cursor_newline(void){
  cursor_col = 0;
  if (cursor_row == TILE_COL_HEIGHT - 1){
    scroll_terminal_one_line();
  } else {
    cursor_row++;
  }
}

static void cancel_pending_wrap(void){
  wrap_pending = false;
}

static void apply_pending_wrap(void){
  if (!wrap_pending){
    return;
  }

  wrap_pending = false;
  advance_cursor_newline();
}

static void handle_carriage_return(void){
  // move to column 0 on the current visible row 
  // and discard any delayed wrap
  cancel_pending_wrap();
  cursor_col = 0;
}

static void handle_backspace(void){
  // move cursor left within the current visible row
  cancel_pending_wrap();
  move_cursor_backward(1);
}

static void handle_tab(void){
  // expand tab into spaces until the next TAB_WIDTH-aligned
  // column. Reusing put_terminal_char() keeps wrapping and scrolling semantics
  // identical to ordinary printable output.
  int spaces = TAB_WIDTH - (cursor_col % TAB_WIDTH);

  if (spaces == 0){
    spaces = TAB_WIDTH;
  }

  for (int i = 0; i < spaces; ++i){
    put_terminal_char(' ');
  }
}

static void put_terminal_char(char c){
  if (c == '\n'){
    // A newline consumes any pending wrap once. Without this, writing a
    // character into the bottom-right cell and then emitting '\n' would scroll
    // twice.
    cancel_pending_wrap();
    advance_cursor_newline();
    return;
  }

  if (c == '\r'){
    handle_carriage_return();
    return;
  }

  if (c == '\b'){
    handle_backspace();
    return;
  }

  if (c == '\t'){
    handle_tab();
    return;
  }

  apply_pending_wrap();
  TILE_FB[cursor_fb_index()] = (short)(((short)current_color << 8) | c);

  if (cursor_col == TILE_ROW_WIDTH - 1){
    if (cursor_row == TILE_COL_HEIGHT - 1){
      wrap_pending = true;
    } else {
      cursor_col = 0;
      cursor_row++;
    }
  } else {
    cursor_col++;
  }
}

static int parse_escape_arg(char* arg, int len){
  int value = 0;
  for (int i = 0; i < len; ++i){
    value = value * 10 + (arg[i] - '0');
  }
  return value;
}

static void apply_sgr_arg(int arg){
  switch (arg){
    case 0:
      current_color = 0xFF;
      break;
    case 30:
      current_color = 0x00;
      break;
    case 31:
      current_color = 0xE0;
      break;
    case 32:
      current_color = 0x1C;
      break;
    case 33:
      current_color = 0xFC;
      break;
    case 34:
      current_color = 0x03;
      break;
    case 35:
      current_color = 0xE3;
      break;
    case 36:
      current_color = 0x1F;
      break;
    case 37:
      current_color = 0xFF;
      break;
  }
}

static bool handle_escape_sequence(char c){
  static char escape_buf[MAX_ESCAPE_SEQUENCE_LENGTH];
  static char escape_args[MAX_ESCAPE_ARGS][MAX_ESCAPE_ARG_LENGTH];
  static char escape_arg_lens[MAX_ESCAPE_ARGS];

  static int escape_index = 0;
  static int escape_arg_num = 0;
  static int escape_arg_index = 0;

  int arg_count;

  // escape sequences should always start with '[' after the initial ESC
  if (escape_index == 0){
    if (c == '['){
      escape_buf[escape_index++] = c;
      return true;
    }

    // invalid escape sequence, reset state
    escape_index = 0;
    escape_arg_num = 0;
    escape_arg_index = 0;
    for (int i = 0; i < MAX_ESCAPE_ARGS; ++i){
      escape_arg_lens[i] = 0;
    }
    return false;
  }

  // escape_arg_num counts separators, 
  // so there may be an argument even when no ';' was seen.
  arg_count = (escape_arg_lens[escape_arg_num] > 0)
    ? (escape_arg_num + 1)
    : escape_arg_num;

  switch (c){
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      // part of an argument, store in buffer
      if (escape_arg_num < MAX_ESCAPE_ARGS &&
          escape_arg_index < MAX_ESCAPE_ARG_LENGTH){
        escape_args[escape_arg_num][escape_arg_index++] = c;
        escape_arg_lens[escape_arg_num] = escape_arg_index;
        return true;
      }
      break;
    }
    case ';': {
      // argument separator
      escape_arg_num++;
      escape_arg_index = 0;

      if (escape_arg_num >= MAX_ESCAPE_ARGS){
        break;
      }

      return true;
    }
    case '?': {
      // part of an argument, store in buffer
      escape_arg_lens[escape_arg_num] = 1;
      escape_args[escape_arg_num++][0] = c;
      escape_arg_index = 0;
      if (escape_arg_num >= MAX_ESCAPE_ARGS){
        break;
      }
      return true;
    }
    case 'A': {
      // move the cursor up by the number of rows specified in the argument (default 1)
      int delta = 1;
      if (arg_count > 0){
        delta = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
     
      cancel_pending_wrap();
      move_cursor_up(delta);
      break;
    }
    case 'B': {
      // move the cursor down by the number of rows specified in the argument (default 1)
      int delta = 1;
      if (arg_count > 0){
        delta = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
      cancel_pending_wrap();
      move_cursor_down(delta);
      break;
    }
    case 'C': {
      // move the cursor forward by the number of columns specified in the argument (default 1)
      int delta = 1;
      if (arg_count > 0){
        delta = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
      cancel_pending_wrap();
      move_cursor_forward(delta);
      break;
    }
    case 'D': {
      // move the cursor backward by the number of columns specified in the argument (default 1)
      int delta = 1;
      if (arg_count > 0){
        delta = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
      cancel_pending_wrap();
      move_cursor_backward(delta);
      break;
    }
    case 'H':
    case 'f': {
      // move the cursor to the specified row and column (default 1,1)
      int row = 0;
      int col = 0;

      if (arg_count >= 1){
        row = parse_escape_arg(escape_args[0], escape_arg_lens[0]) - 1;
      }
      if (arg_count >= 2){
        col = parse_escape_arg(escape_args[1], escape_arg_lens[1]) - 1;
      }

      cancel_pending_wrap();
      cursor_row = clamp_int(row, 0, TILE_COL_HEIGHT - 1);
      cursor_col = clamp_int(col, 0, TILE_ROW_WIDTH - 1);
      break;
    }
    case 's': {
      // Save both visible cursor position and wrap state
      saved_cursor_row = cursor_row;
      saved_cursor_col = cursor_col;
      saved_wrap_pending = wrap_pending;
      break;
    }
    case 'u': {
      // Restore both cursor position and wrap state
      cursor_row = saved_cursor_row;
      cursor_col = saved_cursor_col;
      wrap_pending = saved_wrap_pending;
      break;
    }
    case 'J': {
      // erase part or all of the screen depending on the argument (default 0)
      int arg = (arg_count == 0) ? 0 : parse_escape_arg(escape_args[0], escape_arg_lens[0]);

      if (arg == 0){
        // clear from the cursor to the end of the screen
        for (int col = cursor_col; col < TILE_ROW_WIDTH; ++col){
          TILE_FB[fb_index_for_position(cursor_row, col)] = 0;
        }
        for (int row = cursor_row + 1; row < TILE_COL_HEIGHT; ++row){
          clear_visible_row(row);
        }
      } else if (arg == 1){
        // clear from the start of the screen to the cursor
        for (int row = 0; row < cursor_row; ++row){
          clear_visible_row(row);
        }
        for (int col = 0; col <= cursor_col; ++col){
          TILE_FB[fb_index_for_position(cursor_row, col)] = 0;
        }
      } else if (arg == 2){
        // clear the entire screen
        for (int row = 0; row < TILE_COL_HEIGHT; ++row){
          clear_visible_row(row);
        }
      }
      break;
    }
    case 'K': {
      // erase part or all of the current line depending on the argument (default 0)
      int arg = (arg_count == 0) ? 0 : parse_escape_arg(escape_args[0], escape_arg_lens[0]);

      if (arg == 0){
        // clear from the cursor to the end of the line
        for (int col = cursor_col; col < TILE_ROW_WIDTH; ++col){
          TILE_FB[fb_index_for_position(cursor_row, col)] = 0;
        }
      } else if (arg == 1){
        // clear from the start of the line to the cursor
        for (int col = 0; col <= cursor_col; ++col){
          TILE_FB[fb_index_for_position(cursor_row, col)] = 0;
        }
      } else if (arg == 2){
        // clear the entire line
        clear_visible_row(cursor_row);
      }
      break;
    }
    case 'L': {
      // insert n lines at the current cursor row, pushing lines below down (default 1)
      int n = 1;
      if (arg_count > 0){
        n = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
      n = min_int(n, TILE_COL_HEIGHT - cursor_row);

      for (int j = 0; j < n; ++j){
        for (int row = TILE_COL_HEIGHT - 1; row > cursor_row; --row){
          copy_visible_row(row, row - 1);
        }
        clear_visible_row(cursor_row);
      }
      break;
    }
    case 'M': {
      // delete n lines at the current cursor row, pulling lines below up (default 1)
      int n = 1;
      if (arg_count > 0){
        n = parse_escape_arg(escape_args[0], escape_arg_lens[0]);
      }
      n = min_int(n, TILE_COL_HEIGHT - cursor_row);

      for (int j = 0; j < n; ++j){
        for (int row = cursor_row; row < TILE_COL_HEIGHT - 1; ++row){
          copy_visible_row(row, row + 1);
        }
        clear_visible_row(TILE_COL_HEIGHT - 1);
      }
      break;
    }
    case 'l': {
      // hide the cursor
      if (arg_count < 2){
        break;
      }
      if (escape_args[0][0] != '?'){
        break;
      }
      if (parse_escape_arg(escape_args[1], escape_arg_lens[1]) != 25){
        break;
      }

      cursor_visible = false;
      break;
    }
    case 'h': {
      // show the cursor
      if (arg_count < 2){
        break;
      }
      if (escape_args[0][0] != '?'){
        break;
      }
      if (parse_escape_arg(escape_args[1], escape_arg_lens[1]) != 25){
        break;
      }

      cursor_visible = true;
      break;
    }
    case 'm': {
      // Apply SGR arguments in order so sequences like "0;32m" reset and then
      // apply green instead of silently stopping after the first parameter.
      if (arg_count == 0){
        apply_sgr_arg(0);
        break;
      }

      for (int i = 0; i < arg_count; ++i){
        apply_sgr_arg(parse_escape_arg(escape_args[i], escape_arg_lens[i]));
      }
      break;
    }
  }

  escape_index = 0;
  escape_arg_num = 0;
  escape_arg_index = 0;
  for (int i = 0; i < MAX_ESCAPE_ARGS; ++i){
    escape_arg_lens[i] = 0;
  }

  return false;
}

void render_bytes(int n){
  static bool in_escape_sequence = false;

  erase_cursor();

  for (int i = 0; i < n; ++i){
    char c = terminal_buf[i];
    if (c == '\x1b'){
      in_escape_sequence = true;
      continue;
    }

    if (in_escape_sequence){
      in_escape_sequence = handle_escape_sequence(c);
      continue;
    }

    put_terminal_char(c);
  }

  show_cursor_now();
}

int main(void){
  clear_screen();
  TILE_FB = get_tile_fb();
  load_text_tiles();
  set_tile_scale(0);
  set_vscroll(0);

  scroll_top_row = 0;
  cursor_home();
  wrap_pending = false;
  cursor_blink_on = true;
  last_cursor_blink = get_current_jiffies();
  draw_cursor();

  while (true){
    blink_cursor();

    // read a max of TERMINAL_BUF_SIZE bytes from stdin
    // render the bytes read from stdin to the terminal framebuffer
    int avail = fd_bytes_available(STDIN);
    if (avail > 0){
      unsigned to_read = min_int(avail, TERMINAL_BUF_SIZE);
      int n = read(STDIN, terminal_buf, to_read);
      render_bytes(n);
    }

    sleep(MAIN_LOOP_DELAY_INTERVAL);
  }
}
