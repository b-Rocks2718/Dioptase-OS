/*
 * mousetest: interactive PS/2 mouse demo.
 *
 * Shows a sprite cursor that follows the mouse, a live readout of the last
 * event (position, raw DX/DY, accumulated wheel, held buttons, event count),
 * and a tile canvas to paint on:
 *   left button   paint the cells under the cursor (drags draw lines)
 *   right button  erase
 *   middle button clear the canvas
 *   wheel         cycle the paint color
 *   q / Escape    quit
 *
 * Run it from the shell with VGA enabled (`make run`, then `mousetest`).
 * Motion is relative (docs/mem_map.md "PS/2 mouse input stream"); this
 * program accumulates it into an absolute position clamped to the 640x480
 * screen.
 */

#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/stdbool.h"
#include "../crt/unistd.h"
#include "../crt/vga.h"
#include "../crt/mouse.h"

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

// Tile-layer grid at tile scale 0: 80x60 cells of 8x8 pixels.
#define CELL_SIZE 8

#define TITLE_ROW 0
#define HELP_ROW 1
#define STATUS_ROW 2
#define BUTTON_ROW 3
#define RULE_ROW 4
#define CANVAS_TOP 5

#define CURSOR_SPRITE 0
#define SPRITE_PIXELS 1024

// Custom tile whose pixels all take the tile entry's color byte (0xCXXX).
#define SOLID_TILE 128
#define TILE_PIXELS 64
#define USE_TILE_COLOR_PIXEL ((short)0xC000)

// Sprite pixels with a 0xF top nibble are transparent.
#define TRANSPARENT_PIXEL ((short)0xF000)
// Packed 0x0BGR colors for the cursor.
#define CURSOR_OUTLINE 0x000
#define CURSOR_IDLE 0xFFF
#define CURSOR_LEFT 0x00F
#define CURSOR_RIGHT 0xF80
#define CURSOR_MIDDLE 0x0F0

// RGB332 tile colors for text.
#define TITLE_COLOR 0xFC
#define TEXT_COLOR 0x92
#define VALUE_COLOR 0xFF
#define HELD_COLOR 0x1C
#define RELEASED_COLOR 0x49

#define KEY_ESCAPE 0x1B

// RGB332 paint colors selected with the wheel.
#define PALETTE_SIZE 8
static int palette[PALETTE_SIZE] = {
  0xE0, 0xF4, 0xFC, 0x1C, 0x1F, 0x03, 0xE3, 0xFF,
};

volatile short* TILEMAP;
volatile short* TILE_FB;
volatile short* SPRITES;

// Pointer and display state accumulated from mouse events.
struct MouseState {
  int x;
  int y;
  int buttons;
  int last_dx;
  int last_dy;
  int wheel_total;
  unsigned events;
  int color_index;
};

// Write one cell of the tile framebuffer.
static void put_tile_at(int x, int y, int tile, int color) {
  TILE_FB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | (tile & 0xFF));
}

// Draw a NUL-terminated string starting at a cell; returns the next column.
static int draw_text(int x, int y, char* text, int color) {
  while (*text != '\0') {
    put_tile_at(x, y, *text, color);
    x++;
    text++;
  }
  return x;
}

// Draw a signed decimal right-aligned in `width` cells (sign included).
static void draw_signed_fixed(int x, int y, int value, int width, int color) {
  for (int i = 0; i < width; i++) {
    put_tile_at(x + i, y, ' ', color);
  }
  bool negative = value < 0;
  unsigned magnitude = negative ? (unsigned)(-value) : (unsigned)value;
  int pos = x + width - 1;
  do {
    put_tile_at(pos, y, '0' + (int)(magnitude % 10), color);
    magnitude /= 10;
    pos--;
  } while (magnitude != 0 && pos > x);
  if (negative) {
    put_tile_at(pos, y, '-', color);
  }
}

// Return true when (x, y) is inside the cursor arrow shape: a slender
// triangle with its tip at (0, 0) plus a short diagonal tail.
static bool cursor_inside(int x, int y) {
  if (x < 0 || y < 0 || x >= 32 || y >= 32) {
    return false;
  }
  if (y < 13 && x <= (y * 3) / 4) {
    return true;
  }
  return y >= 9 && y < 18 && x >= y - 8 && x <= y - 6;
}

// Redraw the cursor sprite with an outline and the given fill color.
static void draw_cursor(short fill) {
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      short pixel = TRANSPARENT_PIXEL;
      if (cursor_inside(x, y)) {
        bool edge = !cursor_inside(x - 1, y) || !cursor_inside(x + 1, y) ||
                    !cursor_inside(x, y - 1) || !cursor_inside(x, y + 1);
        pixel = edge ? CURSOR_OUTLINE : fill;
      }
      SPRITES[CURSOR_SPRITE * SPRITE_PIXELS + y * 32 + x] = pixel;
    }
  }
}

// Cursor fill color for the current buttons (left wins, then right, middle).
static short cursor_fill(int buttons) {
  if (buttons & MOUSE_BUTTON_LEFT) return CURSOR_LEFT;
  if (buttons & MOUSE_BUTTON_RIGHT) return CURSOR_RIGHT;
  if (buttons & MOUSE_BUTTON_MIDDLE) return CURSOR_MIDDLE;
  return CURSOR_IDLE;
}

// Blank every canvas cell.
static void clear_canvas(void) {
  for (int y = CANVAS_TOP; y < TILE_COL_HEIGHT; y++) {
    for (int x = 0; x < TILE_ROW_WIDTH; x++) {
      put_tile_at(x, y, ' ', TEXT_COLOR);
    }
  }
}

// Paint (or erase) one canvas cell; cells outside the canvas are ignored.
static void paint_cell(int col, int row, bool erase, int color) {
  if (col < 0 || col >= TILE_ROW_WIDTH || row < CANVAS_TOP || row >= TILE_COL_HEIGHT) {
    return;
  }
  if (erase) {
    put_tile_at(col, row, ' ', TEXT_COLOR);
  } else {
    put_tile_at(col, row, SOLID_TILE, color);
  }
}

// Paint every cell on the line between two cells (Bresenham), so fast drags
// whose events skip cells still leave a continuous stroke.
static void paint_line(int c0, int r0, int c1, int r1, bool erase, int color) {
  int dc = c1 > c0 ? c1 - c0 : c0 - c1;
  int dr = r1 > r0 ? r0 - r1 : r1 - r0;
  int sc = c0 < c1 ? 1 : -1;
  int sr = r0 < r1 ? 1 : -1;
  int err = dc + dr;
  while (true) {
    paint_cell(c0, r0, erase, color);
    if (c0 == c1 && r0 == r1) {
      return;
    }
    int e2 = 2 * err;
    if (e2 >= dr) {
      err += dr;
      c0 += sc;
    }
    if (e2 <= dc) {
      err += dc;
      r0 += sr;
    }
  }
}

// Clamp v into [lo, hi].
static int clamp(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Apply one mouse event: move, track buttons, paint, and cycle color.
static void apply_event(struct MouseState* state, int event) {
  int buttons = MOUSE_BUTTONS(event);
  int pressed = buttons & ~state->buttons;
  int old_col = state->x / CELL_SIZE;
  int old_row = state->y / CELL_SIZE;

  state->last_dx = MOUSE_DX(event);
  state->last_dy = MOUSE_DY(event);
  state->x = clamp(state->x + state->last_dx, 0, SCREEN_WIDTH - 1);
  state->y = clamp(state->y + state->last_dy, 0, SCREEN_HEIGHT - 1);
  state->wheel_total += MOUSE_WHEEL(event);
  state->events++;

  int wheel = MOUSE_WHEEL(event);
  if (wheel != 0) {
    state->color_index =
      ((state->color_index + wheel) % PALETTE_SIZE + PALETTE_SIZE) % PALETTE_SIZE;
  }

  if (pressed & MOUSE_BUTTON_MIDDLE) {
    clear_canvas();
  }

  int col = state->x / CELL_SIZE;
  int row = state->y / CELL_SIZE;
  int color = palette[state->color_index];
  if (buttons & MOUSE_BUTTON_LEFT) {
    paint_line(old_col, old_row, col, row, false, color);
  } else if (buttons & MOUSE_BUTTON_RIGHT) {
    paint_line(old_col, old_row, col, row, true, color);
  }

  if (buttons != state->buttons) {
    draw_cursor(cursor_fill(buttons));
  }
  state->buttons = buttons;
}

// Draw one "[X]" button indicator, highlighted while held.
static int draw_button(int x, char* label, bool held) {
  return draw_text(x, BUTTON_ROW, label, held ? HELD_COLOR : RELEASED_COLOR) + 1;
}

// Redraw the live status rows.
static void draw_status(struct MouseState* state) {
  int x = draw_text(0, STATUS_ROW, "x=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, state->x, 4, VALUE_COLOR);
  x = draw_text(x + 5, STATUS_ROW, "y=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, state->y, 4, VALUE_COLOR);
  x = draw_text(x + 6, STATUS_ROW, "last dx=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, state->last_dx, 5, VALUE_COLOR);
  x = draw_text(x + 6, STATUS_ROW, "dy=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, state->last_dy, 5, VALUE_COLOR);
  x = draw_text(x + 7, STATUS_ROW, "wheel=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, state->wheel_total, 6, VALUE_COLOR);
  x = draw_text(x + 8, STATUS_ROW, "events=", TEXT_COLOR);
  draw_signed_fixed(x, STATUS_ROW, (int)state->events, 8, VALUE_COLOR);

  x = draw_text(0, BUTTON_ROW, "buttons: ", TEXT_COLOR);
  x = draw_button(x, "[L]", (state->buttons & MOUSE_BUTTON_LEFT) != 0);
  x = draw_button(x, "[M]", (state->buttons & MOUSE_BUTTON_MIDDLE) != 0);
  x = draw_button(x, "[R]", (state->buttons & MOUSE_BUTTON_RIGHT) != 0);
  x = draw_text(x + 3, BUTTON_ROW, "paint color: ", TEXT_COLOR);
  for (int i = 0; i < 4; i++) {
    put_tile_at(x + i, BUTTON_ROW, SOLID_TILE, palette[state->color_index]);
  }
}

// Draw the static title, help, and separator rows.
static void draw_static_rows(void) {
  draw_text(0, TITLE_ROW, "Dioptase PS/2 mouse test", TITLE_COLOR);
  draw_text(0, HELP_ROW,
    "L: paint   R: erase   M: clear   wheel: color   q/Esc: quit", TEXT_COLOR);
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    put_tile_at(x, RULE_ROW, '-', RELEASED_COLOR);
  }
}

// Read a piped input byte when available, otherwise poll the keyboard.
static int read_input_event(void) {
  int available = fd_bytes_available(STDIN);
  if (available > 0) {
    unsigned char byte;
    if (read(STDIN, &byte, 1) == 1) {
      return byte;
    }
    return 0;
  }
  if (available < 0) {
    return getkey();
  }
  return 0;
}

// Yield until the VGA frame counter advances, pacing redraws to the display.
static void wait_for_next_frame(void) {
  unsigned frame = get_vga_frame_counter();
  while (get_vga_frame_counter() == frame) {
    yield();
  }
}

int main(void) {
  // Hide the terminal cursor and clear it before taking over the tile layer.
  puts("\x1b[?25l\x1b[2J");

  TILE_FB = get_tile_fb();
  TILEMAP = get_tilemap();
  SPRITES = get_spritemap();

  load_text_tiles();
  clear_screen();
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  for (int i = 0; i < TILE_PIXELS; i++) {
    TILEMAP[SOLID_TILE * TILE_PIXELS + i] = USE_TILE_COLOR_PIXEL;
  }

  struct MouseState state;
  state.x = SCREEN_WIDTH / 2;
  state.y = SCREEN_HEIGHT / 2;
  state.buttons = 0;
  state.last_dx = 0;
  state.last_dy = 0;
  state.wheel_total = 0;
  state.events = 0;
  state.color_index = 0;

  // Discard motion queued while no program was reading the mouse.
  while (getmouse() != 0) {
  }

  set_sprite_scale(CURSOR_SPRITE, 0);
  draw_cursor(cursor_fill(0));
  draw_static_rows();
  clear_canvas();

  while (true) {
    int key = read_input_event();
    if (key == 'q' || key == 'Q' || key == KEY_ESCAPE) {
      break;
    }

    int event;
    while ((event = getmouse()) != 0) {
      apply_event(&state, event);
    }

    draw_status(&state);
    set_sprite_coords(CURSOR_SPRITE, state.x, state.y);
    wait_for_next_frame();
  }

  set_sprite_coords(CURSOR_SPRITE, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  return 0;
}
