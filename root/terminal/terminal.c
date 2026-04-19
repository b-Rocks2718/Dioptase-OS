#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"

#define TILE_ROW_WIDTH 80
#define TILE_COL_HEIGHT 60

#define TILE_HEIGHT 8
#define TILE_WIDTH 8

#define FB_NUM_TILES 4800
#define SQUARE_TILE 0x7F

#define CURSOR_BLINK_INTERVAL 100
#define MAIN_LOOP_DELAY_INTERVAL 10

#define TERMINAL_BUF_SIZE 128

short* TILE_FB = NULL;
int cursor_index = 0;
char terminal_buf[TERMINAL_BUF_SIZE];

void blink_cursor(void){
  static unsigned last_blink = 0;
  unsigned now = get_current_jiffies();
  if (now - last_blink >= CURSOR_BLINK_INTERVAL){
    last_blink = now;
    // toggle cursor tile at current vga_index
    TILE_FB[cursor_index] ^= 0xFF00; // hardcore cursor to white for now
  }
}

bool handle_escape_sequence(char c){
  // TODO: parse escape sequence
  return false;
}

void render_bytes(int n){
  static bool scrolling = false;
  static bool was_newline = false;
  static char current_color = 0xFF;
  static bool in_escape_sequence = false;

  for (int i = 0; i < n; ++i){
  char c = terminal_buf[i];
    if (c == '\x1b'){
      // escape sequence
      in_escape_sequence = true;
      continue;
    }

    if (in_escape_sequence){
      in_escape_sequence = handle_escape_sequence(c);
      continue;
    }

    if (was_newline && scrolling){
      // clear the new line we're about to write on if we just scrolled
      for (int i = 0; i < TILE_ROW_WIDTH; ++i){
        TILE_FB[cursor_index + i] = 0;
      }
      // scroll up by one line
      move_vscroll(-TILE_HEIGHT);
      was_newline = false;
    }

    if (c == '\n'){
      TILE_FB[cursor_index++] = 0;
      // round up to next row
      cursor_index = ((cursor_index + TILE_ROW_WIDTH - 1) / TILE_ROW_WIDTH) * TILE_ROW_WIDTH;

      was_newline = true;
    } else {
      // write character normally
      TILE_FB[cursor_index++] = (short)(((short)current_color << 8) | c);
    }

    if (cursor_index >= FB_NUM_TILES) {
      // screen is full, scroll up by one line
      cursor_index -= FB_NUM_TILES;
      scrolling = true;
    }
  }
}

int main(void){
  clear_screen();
  TILE_FB = get_tile_fb();
  load_text_tiles();
  set_tile_scale(0);

  while(true){
    blink_cursor();

    // read a max of TERMINAL_BUF_SIZE bytes from stdin
    // render the bytes read from stdin to the terminal framebuffer
    int avail = fd_bytes_available(STDIN);
    if (avail > 0){
      unsigned to_read = avail > TERMINAL_BUF_SIZE ? TERMINAL_BUF_SIZE : avail;
      int n = read(STDIN, terminal_buf, to_read);
      render_bytes(n);

      // draw the cursor at the current cursor_index
      TILE_FB[cursor_index] = SQUARE_TILE;
    }

    sleep(MAIN_LOOP_DELAY_INTERVAL);
  }
}
