/*
 * Powderworks: a falling-sand alchemy lab for Dioptase.
 *
 * Paint sand, water, oil, lava, fire and more into a stone tray and watch
 * them fall, flow, float, burn, dissolve, quench, melt and grow. The world
 * lives in the pixel framebuffer (docs/mem_map.md: 320x236 user pixels in
 * 0x0BGR, drawn under the tile layer), the HUD and menus live in the tile
 * framebuffer as text, and the brush is hardware sprite 0. Input is the
 * PS/2 mouse (getmouse, root/crt/mouse.h) plus raw keyboard records from
 * the terminal (docs/terminal.md), with a cooked-byte fallback. Sound is
 * the 25 kHz mono audio device (docs/syscalls.md "Audio"). Scenes persist
 * through the ordinary file syscalls. Signed on the title screen:
 * Muse Code, powered by Meta Muse Spark.
 */

#include "sim.h"

#include "../crt/fcntl.h"
#include "../crt/mouse.h"
#include "../crt/print.h"
#include "../crt/ps2.h"
#include "../crt/stdbool.h"
#include "../crt/string.h"
#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/vga.h"

/* Program states: the title runs the demo scene behind its menu. */
#define ST_TITLE 0
#define ST_PLAY 1
#define ST_HELP 2

/* Tile indices provided by load_text_tiles (kernel/print.c). */
#define TILE_SOLID 127
#define TILE_CLEAR 255

/* Tile colors (RGB332 bytes) for HUD text and markers. */
#define COL_TITLE 0xFE
#define COL_TEXT 0x92
#define COL_DIM 0x49
#define COL_HOT 0xF0
#define COL_GOOD 0x3E
#define COL_BAD 0xE9
#define COL_MARK 0xFF

/* Brush radii offered by the wheel and the B key, largest last. */
#define BRUSH_STEPS 5

/* Save file layout: a 16-byte header followed by the raw cell grid. */
#define SAVE_PATH "/powder/scene.pwd"
#define SAVE_MAGIC0 'P'
#define SAVE_MAGIC1 'W'

/* Palette order; values are element codes, with BRUSH_ERASE last. */
static int palette[PALETTE_COUNT] = {
  EL_SAND, EL_WATER, EL_OIL, EL_LAVA, EL_FIRE, EL_EMBER, EL_WOOD,
  EL_STONE, EL_ACID, EL_SNOW, EL_PLANT, EL_SMOKE, BRUSH_ERASE,
};

/* Framebuffer and tilemap mappings, resolved once at startup. */
static volatile short* PX;
static volatile int* PXW;
static volatile short* TFB;
static volatile short* TMAP;
static volatile short* SPR;

/* Handwritten row publisher in powder_asm.s; see that file for its contract. */
void powder_publish_row(volatile int* fb, short* colors, short* prev);

/* Scratch row of colors filled by sim_row_colors for the publisher. */
static short row_colors[SIM_W];

/* Last color published per cell, so idle cells are not rewritten. */
static short prev_color[SIM_CELLS];

/* Interaction state shared by the mouse, keyboard, and attract painter. */
static int state = ST_TITLE;
static int help_return = ST_TITLE;
static int paused = 0;
static int sound_on = 1;
static int audio_owned = 0;
static int selected = 0;
static int brush_step = 2;
static int cursor_x = 80;
static int cursor_y = 40;
/* Absolute pointer position in 640x480 screen pixels; one cell is 4x4. */
static int cursor_sx = 320;
static int cursor_sy = 160;
static int mouse_buttons = 0;
static int key_paint = 0;
static unsigned message_until = 0;
static char message[80];
static int attract = 1;
static int attract_x = 80;
static unsigned attract_timer = 0;
static int raw_mode = 0;
static unsigned hud_dirty = 1;
static unsigned last_count_steps = 0;

/* Previous mouse buttons, for middle-click edge detection. */
static int prev_buttons = 0;

/* Key state by guest keycode, plus cooked-mode latch countdowns. */
static unsigned char key_down[256];
static unsigned char key_latch[256];
/* Press edges latched until the next snapshot, so taps shorter than a frame still count. */
static unsigned char key_edge[256];

/* Copy the current key state into the edge-detection snapshot. */
static void snapshot_keys(void) {
  for (int i = 0; i < 256; i++) key_edge[i] = 0;
}

/* True only on the first frame a key is down. */
static int key_pressed(int code) {
  return key_edge[code & 0xFF] != 0;
}

/* Write one tile-framebuffer cell from a tile index and RGB332 color. */
static void put_tile(int x, int y, int tile, int color) {
  if (x < 0 || x >= TILE_ROW_WIDTH || y < 0 || y >= TILE_COL_HEIGHT) return;
  TFB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | (tile & 0xFF));
}

/* Draw a NUL-terminated string as text tiles; returns the next column. */
static int draw_text(int x, int y, char* text, int color) {
  while (*text != '\0') {
    put_tile(x, y, (unsigned char)*text, color);
    x++;
    text++;
  }
  return x;
}

/* Draw an unsigned decimal right-aligned in a fixed number of cells. */
static void draw_unsigned(int x, int y, unsigned value, int width, int color) {
  for (int i = 0; i < width; i++) put_tile(x + i, y, ' ', color);
  int pos = x + width - 1;
  if (value == 0) {
    put_tile(pos, y, '0', color);
    return;
  }
  while (value != 0 && pos >= x) {
    put_tile(pos, y, '0' + (int)(value % 10), color);
    value /= 10;
    pos--;
  }
}

/* Fill one tile row with transparent cells so the pixel layer shows through. */
static void clear_tile_row(int y) {
  for (int x = 0; x < TILE_ROW_WIDTH; x++) put_tile(x, y, TILE_CLEAR, 0);
}

/* Show a transient status message in the HUD until a future sim step. */
static void show_message(char* text, unsigned steps_ahead) {
  int i = 0;
  while (text[i] != '\0' && i < 79) {
    message[i] = text[i];
    i++;
  }
  message[i] = '\0';
  message_until = sim_steps + steps_ahead;
  hud_dirty = 1;
}

/* Publish the simulation to the pixel framebuffer, skipping unchanged cells. */
static void render_sim(void) {
  volatile int* fb = PXW;
  for (int y = 0; y < SIM_H; y++) {
    sim_row_colors(y, row_colors);
    powder_publish_row(fb, row_colors, &prev_color[(y << 7) + (y << 5)]);
    fb += 320;
  }
}

/* Invalidate the render cache so the next frame republishes every cell. */
static void render_invalidate(void) {
  for (int i = 0; i < SIM_CELLS; i++) prev_color[i] = (short)0xFFFF;
}

/* Draw the two HUD rows: identity and counts above, brush state below. */
static void draw_hud(void) {
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    put_tile(x, 0, ' ', COL_TEXT);
    put_tile(x, 1, ' ', COL_TEXT);
  }
  draw_text(1, 0, "POWDERWORKS", COL_TITLE);
  int x = draw_text(14, 0, "cells", COL_DIM);
  int total = 0;
  for (int e = 1; e < EL_COUNT; e++) total += sim_counts[e];
  draw_unsigned(x + 1, 0, (unsigned)total, 5, COL_TEXT);
  x = draw_text(x + 7, 0, "fire", COL_DIM);
  draw_unsigned(x + 1, 0, (unsigned)sim_counts[EL_FIRE], 3, COL_HOT);
  x = draw_text(x + 5, 0, "step", COL_DIM);
  draw_unsigned(x + 1, 0, sim_steps, 6, COL_TEXT);
  if (paused) draw_text(70, 0, "PAUSED", COL_HOT);
  else if (!sound_on) draw_text(70, 0, "MUTED", COL_DIM);

  int element = palette[selected];
  x = draw_text(1, 1, "brush:", COL_DIM);
  x = draw_text(x + 1, 1, sim_element_name(element), COL_GOOD);
  x = draw_text(x + 1, 1, "size", COL_DIM);
  draw_unsigned(x + 1, 1, (unsigned)(brush_step + 1), 1, COL_TEXT);
  if (sim_steps < message_until) {
    draw_text(24, 1, message, COL_HOT);
  } else {
    draw_text(24, 1, "mouse paints - wheel size - H help - Q quit", COL_DIM);
  }
}

/* Draw the palette strip: swatches on row 58, key labels on row 59. */
static void draw_palette(void) {
  clear_tile_row(57);
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    put_tile(x, 58, ' ', COL_TEXT);
    put_tile(x, 59, ' ', COL_TEXT);
  }
  for (int i = 0; i < PALETTE_COUNT; i++) {
    int element = palette[i];
    int col = 2 + i * 6;
    int tile_color = element == BRUSH_ERASE ? 0x49 : sim_element_tile_color(element);
    put_tile(col, 58, TILE_SOLID, tile_color);
    put_tile(col + 1, 58, TILE_SOLID, tile_color);
    put_tile(col + 2, 58, TILE_SOLID, tile_color);
    char label[4];
    char* name = sim_element_name(element);
    label[0] = name[0];
    label[1] = name[1];
    label[2] = name[2];
    label[3] = '\0';
    draw_text(col, 59, label, i == selected ? COL_MARK : COL_DIM);
    if (i == selected) put_tile(col + 1, 57, 'v', COL_MARK);
  }
}

/* Draw a bordered text box of n lines centered on the tile screen. */
static void draw_text_box(int top, char** lines, int count, int title_color) {
  int width = 64;
  int left = (TILE_ROW_WIDTH - width) / 2;
  for (int r = 0; r < count + 2; r++) {
    for (int x = 0; x < TILE_ROW_WIDTH; x++) {
      if (x >= left && x < left + width) put_tile(x, top + r, ' ', COL_TEXT);
    }
  }
  for (int x = left; x < left + width; x++) {
    put_tile(x, top, '-', COL_DIM);
    put_tile(x, top + count + 1, '-', COL_DIM);
  }
  for (int i = 0; i < count; i++) {
    draw_text(left + 2, top + 1 + i, lines[i], i == 0 ? title_color : COL_TEXT);
  }
}

/* Title menu lines, including the signature required of every demo. */
static char* title_lines[10] = {
  "                P O W D E R W O R K S",
  "         a falling-sand alchemy lab for Dioptase",
  "",
  "  Paint with the mouse. Sand piles, water flows, oil",
  "  floats, lava quenches to stone, acid eats the tray,",
  "  vines drink and climb. Every reaction is live.",
  "",
  "  ENTER or CLICK  begin      N  new showcase scene",
  "  H  handbook               Q  return to the shell",
  "     by Muse Code - powered by Meta Muse Spark",
};

/* Handbook lines shown over a frozen world. */
static char* help_lines[16] = {
  "  POWDERWORKS HANDBOOK",
  "  mouse          move brush      wheel / B   brush size",
  "  left button    paint           right btn   erase",
  "  WASD / arrows  move brush      SPACE       paint",
  "  1..9 0 [ ]     choose element  V vine  G smoke  X erase",
  "  P pause   C clear tray   N showcase   O save   L load",
  "  M sound   H close help   Q quit to shell",
  "",
  "  SAND sinks in water - OIL floats - LAVA + WATER = STONE",
  "  FIRE eats wood, oil and vines, then dies to SMOKE",
  "  EMBERs rain sparks and cool to ash - ACID dissolves",
  "  SNOW melts by heat - VINE grows toward water",
  "  Click a palette swatch below to choose an element.",
  "  Scenes save to /powder/scene.pwd and survive reboots.",
  "",
  "     by Muse Code - powered by Meta Muse Spark",
};

/* Redraw every tile-layer element when the HUD is marked dirty. */
static void draw_chrome(void) {
  draw_hud();
  draw_palette();
  if (state == ST_TITLE) {
    draw_text_box(20, title_lines, 10, COL_TITLE);
  } else if (state == ST_HELP) {
    draw_text_box(16, help_lines, 16, COL_GOOD);
  }
  hud_dirty = 0;
}

/* Erase the menu box area by returning its cells to transparency. */
static void clear_chrome_boxes(void) {
  for (int y = 14; y < 36; y++) clear_tile_row(y);
}

/* Redraw the brush cursor sprite for the current selection and size. */
static void draw_cursor_sprite(void) {
  int element = palette[selected];
  short fill = element == BRUSH_ERASE
                 ? (short)0xEEE
                 : sim_element_color(element, 2);
  int radius = 6 + brush_step * 2;
  if (radius > 14) radius = 14;
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      int dx = x - 16;
      int dy = y - 16;
      int d2 = dx * dx + dy * dy;
      short pixel = (short)0xF000;
      if (d2 <= radius * radius && d2 >= (radius - 2) * (radius - 2)) {
        pixel = mouse_buttons != 0 ? (short)0xFFF : fill;
      }
      if (x >= 15 && x <= 17 && y >= 15 && y <= 17) pixel = (short)0xFFF;
      SPR[y * 32 + x] = pixel;
    }
  }
}

/* One pending sound effect: remaining samples and a noise/tone selector. */
static int sfx_samples = 0;
static int sfx_kind = 0;
static unsigned sfx_phase = 0;

/* Queue a short effect: 1 ignite crackle, 2 quench hiss, 3 UI click. */
static void sfx_start(int kind, int samples) {
  if (!sound_on || !audio_owned) return;
  sfx_kind = kind;
  sfx_samples = samples;
  sfx_phase = 0;
}

/* Feed the audio ring one small chunk; all device calls are non-blocking. */
static void audio_pump(void) {
  if (!sound_on || !audio_owned) return;
  int crackle = sim_counts[EL_FIRE] > 6;
  if (sfx_samples <= 0 && !crackle) return;
  short chunk[256];
  int n = 256;
  if (sfx_samples > 0 && sfx_samples < n) n = sfx_samples;
  for (int i = 0; i < n; i++) {
    int value = 0;
    sfx_phase++;
    if (sfx_kind == 3) {
      value = ((sfx_phase & 16) != 0 ? 5000 : -5000) / 2;
    } else {
      unsigned r = sim_rand();
      value = (int)(r & 0x3FFF) - 8192;
      if (sfx_kind == 2) value /= 2; /* A hiss sits under the crackle. */
      value = value * 3 / 4;
    }
    if (sfx_samples <= 0 && crackle) {
      unsigned r = sim_rand();
      value = ((int)(r & 0x1FFF) - 4096) / 4; /* Distant fire, kept quiet. */
    }
    chunk[i] = (short)value;
  }
  int written = audio_write(chunk, (unsigned)(n * 2));
  if (written > 0 && sfx_samples > 0) {
    sfx_samples -= written / 2;
    if (sfx_samples < 0) sfx_samples = 0;
  }
}

/* Write the whole buffer through the 1024-byte syscall clamp, in pieces. */
static int write_all(int fd, unsigned char* data, int count) {
  int done = 0;
  while (done < count) {
    int piece = count - done;
    if (piece > 512) piece = 512;
    int wrote = write(fd, data + done, (unsigned)piece);
    if (wrote <= 0) return -1;
    done += wrote;
  }
  return 0;
}

/* Read exactly count bytes or fail; short reads at EOF are failures here. */
static int read_all(int fd, unsigned char* data, int count) {
  int done = 0;
  while (done < count) {
    int piece = count - done;
    if (piece > 512) piece = 512;
    int got = read(fd, data + done, (unsigned)piece);
    if (got <= 0) return -1;
    done += got;
  }
  return 0;
}

/* Save the cell grid with a small magic header; reports through the HUD. */
static void save_scene(void) {
  int fd = open(SAVE_PATH);
  if (fd < 0) {
    show_message("SAVE FAILED - NO /powder DIR?", 90);
    return;
  }
  unsigned char header[16];
  header[0] = (unsigned char)SAVE_MAGIC0;
  header[1] = (unsigned char)SAVE_MAGIC1;
  header[2] = (unsigned char)SIM_W;
  header[3] = (unsigned char)SIM_H;
  for (int i = 4; i < 16; i++) header[i] = 0;
  int ok = write_all(fd, header, 16) == 0 &&
           write_all(fd, sim_cells, SIM_CELLS) == 0;
  close(fd);
  show_message(ok ? "SCENE SAVED TO /powder/scene.pwd" : "SAVE FAILED", 90);
  if (ok) sfx_start(3, 1200);
}

/* Load a saved grid, rejecting foreign or truncated files. */
static void load_scene(void) {
  int fd = open_existing(SAVE_PATH);
  if (fd < 0) {
    show_message("NO SAVED SCENE YET - PRESS O TO SAVE", 90);
    return;
  }
  unsigned char header[16];
  int ok = read_all(fd, header, 16) == 0 &&
           header[0] == (unsigned char)SAVE_MAGIC0 &&
           header[1] == (unsigned char)SAVE_MAGIC1 &&
           header[2] == (unsigned char)SIM_W &&
           header[3] == (unsigned char)SIM_H &&
           read_all(fd, sim_cells, SIM_CELLS) == 0;
  close(fd);
  if (ok) {
    for (int i = 0; i < SIM_CELLS; i++) {
      if (sim_cells[i] >= EL_COUNT) sim_cells[i] = EL_EMPTY;
      sim_aux[i] = sim_cells[i] == EL_LAVA ? 255 : 1;
    }
    sim_recount();
    render_invalidate();
    show_message("SCENE LOADED", 90);
    sfx_start(3, 1200);
  } else {
    show_message("SAVE FILE IS DAMAGED - IGNORING IT", 90);
  }
}

/* Advance the title-screen attract painter: a wandering elemental rain. */
static void attract_step(void) {
  attract_timer++;
  if ((attract_timer % 40) == 0) {
    attract_x += (int)(sim_rand() % 41) - 20;
    if (attract_x < 12) attract_x = 12;
    if (attract_x > 148) attract_x = 148;
  }
  if ((attract_timer % 7) == 0) {
    int element = palette[(attract_timer / 90) % 6];
    sim_paint(attract_x, 10, 2, element);
  }
  if ((attract_timer % 251) == 0) sim_paint(attract_x, 24, 1, EL_EMBER);
}

/* Derive the brush cell from the absolute screen pointer position. */
static void sync_cursor_cell(void) {
  cursor_x = cursor_sx / 4;
  cursor_y = cursor_sy / 4;
  if (cursor_x < PLAY_X0) cursor_x = PLAY_X0;
  if (cursor_x > PLAY_X1) cursor_x = PLAY_X1;
  if (cursor_y < PLAY_Y0) cursor_y = PLAY_Y0;
  if (cursor_y > SIM_H - 3) cursor_y = SIM_H - 3;
}

/* Poll the mouse once per frame and fold events into cursor and buttons. */
static void poll_mouse(void) {
  int event = getmouse();
  while (event != 0) {
    cursor_sx += MOUSE_DX(event);
    cursor_sy += MOUSE_DY(event);
    int wheel = MOUSE_WHEEL(event);
    if (cursor_sx < 0) cursor_sx = 0;
    if (cursor_sx > 639) cursor_sx = 639;
    if (cursor_sy < 0) cursor_sy = 0;
    if (cursor_sy > 479) cursor_sy = 479;
    if (wheel != 0) {
      brush_step += wheel > 0 ? 1 : -1;
      if (brush_step < 0) brush_step = 0;
      if (brush_step >= BRUSH_STEPS) brush_step = BRUSH_STEPS - 1;
      draw_cursor_sprite();
      hud_dirty = 1;
    }
    mouse_buttons = MOUSE_BUTTONS(event);
    event = getmouse();
  }
  sync_cursor_cell();
}

/* Fold one keyboard event into the key-state tables. */
static void key_event(int code, int pressed) {
  if (code < 0 || code > 255) return;
  key_down[code] = pressed ? 1 : 0;
  if (pressed) {
    key_latch[code] = 10;
    key_edge[code] = 1;
  }
}

/* Drain keyboard input in whichever form the terminal is providing. */
static void poll_keyboard(void) {
  if (raw_mode) {
    int avail = fd_bytes_available(STDIN);
    while (avail >= 2) {
      unsigned char record[2];
      if (read(STDIN, record, 2) != 2) break;
      key_event(record[0], record[1] == 0);
      avail = fd_bytes_available(STDIN);
      if (avail < 0) break;
    }
  } else {
    int avail = fd_bytes_available(STDIN);
    while (avail > 0) {
      unsigned char byte = 0;
      if (read(STDIN, &byte, 1) != 1) break;
      key_event(byte, 1);
      avail = fd_bytes_available(STDIN);
      if (avail < 0) break;
    }
    if (avail < 0) {
      int key = getkey();
      while (key != 0) {
        key_event(key & 0xFF, (key & 0x100) == 0);
        key = getkey();
      }
    }
  }
  for (int i = 0; i < 256; i++) {
    if (key_latch[i] > 0) {
      key_latch[i]--;
      if (key_latch[i] == 0 && !raw_mode) key_down[i] = 0;
    }
  }
}

/* Palette index for a number-row key, or -1 when the key picks nothing. */
static int palette_for_key(int code) {
  if (code >= '1' && code <= '9') return code - '1';
  if (code == '0') return 9;
  return -1;
}

/* Select a palette entry and refresh every brush visual. */
static void select_palette(int index) {
  if (index < 0 || index >= PALETTE_COUNT || index == selected) return;
  selected = index;
  draw_cursor_sprite();
  hud_dirty = 1;
}

/* Begin play from the title, keeping the showcased world on screen. */
static void start_play(void) {
  state = ST_PLAY;
  attract = 0;
  clear_chrome_boxes();
  show_message("PAINT! LEFT=PAINT RIGHT=ERASE WHEEL=SIZE", 120);
  sfx_start(3, 900);
  hud_dirty = 1;
}

/* Open or close the handbook over the current state. */
static void toggle_help(void) {
  if (state == ST_HELP) {
    state = help_return;
    clear_chrome_boxes();
  } else {
    help_return = state;
    state = ST_HELP;
  }
  hud_dirty = 1;
}

/* Handle held movement and painting plus every edge-triggered command. */
static int handle_input(void) {
  /* Palette clicks select in any state and never start or paint. */
  int over_palette = cursor_sy >= 57 * 8;
  if ((mouse_buttons & MOUSE_BUTTON_LEFT) != 0 && over_palette) {
    int col = cursor_sx / 8;
    int index = (col - 2) / 6;
    if (index >= 0 && index < PALETTE_COUNT && col >= 2) select_palette(index);
    return 0;
  }
  if (state == ST_TITLE) {
    if (key_pressed(0x0D) || key_pressed(' ') ||
        ((mouse_buttons & MOUSE_BUTTON_LEFT) != 0 && !over_palette)) {
      start_play();
    } else if (key_pressed('h') || key_pressed('H')) {
      toggle_help();
    } else if (key_pressed('n') || key_pressed('N')) {
      sim_demo_scene();
      render_invalidate();
      show_message("SHOWCASE REBUILT", 90);
    } else if (key_pressed('q') || key_pressed('Q') || key_pressed(0x1B)) {
      return 1;
    }
    return 0;
  }
  if (state == ST_HELP) {
    if (key_pressed('h') || key_pressed('H') || key_pressed(0x1B) ||
        key_pressed(0x0D)) {
      toggle_help();
    }
    return 0;
  }
  /* Play state: movement, painting, and commands. */
  int move = (key_down['w'] || key_down['W'] || key_down[KEY_UP]) ? -4 : 0;
  if (key_down['s'] || key_down['S'] || key_down[KEY_DOWN]) move = 4;
  cursor_sy += move;
  move = (key_down['a'] || key_down['A'] || key_down[KEY_LEFT]) ? -4 : 0;
  if (key_down['d'] || key_down['D'] || key_down[KEY_RIGHT]) move = 4;
  cursor_sx += move;
  if (cursor_sx < 0) cursor_sx = 0;
  if (cursor_sx > 639) cursor_sx = 639;
  if (cursor_sy < 0) cursor_sy = 0;
  if (cursor_sy > 479) cursor_sy = 479;
  sync_cursor_cell();

  for (int code = 0; code < 256; code++) {
    if (!key_pressed(code)) continue;
    int pick = palette_for_key(code);
    if (pick >= 0) select_palette(pick);
  }
  if (key_pressed('[')) select_palette(selected - 1);
  if (key_pressed(']')) select_palette(selected + 1);
  if (key_pressed('v') || key_pressed('V')) select_palette(10);
  if (key_pressed('g') || key_pressed('G')) select_palette(11);
  if (key_pressed('x') || key_pressed('X')) select_palette(12);
  if (key_pressed('b') || key_pressed('B')) {
    brush_step = (brush_step + 1) % BRUSH_STEPS;
    draw_cursor_sprite();
    hud_dirty = 1;
  }
  if (key_pressed('p') || key_pressed('P')) {
    paused = !paused;
    show_message(paused ? "PAUSED - P RESUMES" : "RUNNING", 60);
  }
  if (key_pressed('c') || key_pressed('C')) {
    sim_clear();
    render_invalidate();
    show_message("TRAY CLEARED", 60);
  }
  if (key_pressed('n') || key_pressed('N')) {
    sim_demo_scene();
    render_invalidate();
    show_message("SHOWCASE SCENE - BURN THE BRIDGE!", 90);
  }
  if (key_pressed('o') || key_pressed('O')) save_scene();
  if (key_pressed('l') || key_pressed('L')) load_scene();
  if (key_pressed('m') || key_pressed('M')) {
    sound_on = !sound_on;
    if (!sound_on && audio_owned) {
      audio_close();
      audio_owned = 0;
    } else if (sound_on && !audio_owned) {
      audio_owned = audio_open() == 0;
    }
    show_message(sound_on ? "SOUND ON" : "SOUND MUTED", 60);
  }
  if (key_pressed('h') || key_pressed('H')) toggle_help();
  if (key_pressed('q') || key_pressed('Q')) return 1;

  /* The middle button samples the element under the brush. */
  if ((mouse_buttons & MOUSE_BUTTON_MIDDLE) != 0 &&
      (prev_buttons & MOUSE_BUTTON_MIDDLE) == 0) {
    int under = sim_at(cursor_x, cursor_y);
    for (int i = 0; i < PALETTE_COUNT; i++) {
      if (palette[i] == under) select_palette(i);
    }
  }
  prev_buttons = mouse_buttons;

  key_paint = key_down[' '] || key_down[0x0D];
  if (!over_palette && !paused) {
    if ((mouse_buttons & MOUSE_BUTTON_LEFT) != 0 || key_paint) {
      sim_paint(cursor_x, cursor_y, brush_step, palette[selected]);
    } else if ((mouse_buttons & MOUSE_BUTTON_RIGHT) != 0) {
      sim_paint(cursor_x, cursor_y, brush_step, BRUSH_ERASE);
    }
  }
  return 0;
}

/* Wait until the VGA frame counter advances, yielding the CPU meanwhile. */
static unsigned wait_frame(unsigned last) {
  unsigned now = get_vga_frame_counter();
  while (now == last) {
    sleep(1);
    now = get_vga_frame_counter();
  }
  return now;
}

/* Claim every display mapping and put the layers into a known state. */
static int setup_display(void) {
  PX = get_pixel_fb();
  PXW = (volatile int*)PX;
  TFB = get_tile_fb();
  TMAP = get_tilemap();
  SPR = get_spritemap();
  if (PX == (short*)-1 || TFB == (short*)-1 || TMAP == (short*)-1 ||
      SPR == (short*)-1) {
    puts("***FAIL powder display mapping unavailable\n");
    return -1;
  }
  set_pixel_scale(0);
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  load_text_tiles();
  for (int i = 0; i < FB_NUM_TILES; i++) TFB[i] = (short)((0 << 8) | TILE_CLEAR);
  for (int s = 0; s < NUM_SPRITES; s++) {
    set_sprite_scale((unsigned)s, 0);
    set_sprite_coords((unsigned)s, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  }
  render_invalidate();
  return 0;
}

/* Deterministic guest check: rules, display mappings, audio, persistence. */
static int run_selftest(void) {
  if (setup_display() != 0) return 1;
  PX[100] = 0x0ABC;
  if (PX[100] != 0x0ABC) {
    puts("***FAIL powder selftest pixel readback\n");
    return 1;
  }
  TFB[100] = (short)((COL_GOOD << 8) | 'P');
  if ((TFB[100] & 0xFF) != 'P') {
    puts("***FAIL powder selftest tile readback\n");
    return 1;
  }
  SPR[100] = 0x0123;
  if (SPR[100] != 0x0123) {
    puts("***FAIL powder selftest sprite readback\n");
    return 1;
  }
  sim_init(12345);
  sim_paint(80, 20, 4, EL_SAND);
  int sand_before = sim_counts[EL_SAND];
  for (int i = 0; i < 200; i++) sim_step();
  sim_recount();
  if (sim_counts[EL_SAND] != sand_before || sim_at(80, 20) != EL_EMPTY) {
    puts("***FAIL powder selftest sand fall\n");
    return 1;
  }
  sim_paint(80, 100, 5, EL_LAVA);
  for (int i = 0; i < 50; i++) sim_step();
  int stone_before = sim_counts[EL_STONE];
  sim_paint(80, 92, 4, EL_WATER);
  for (int i = 0; i < 200; i++) sim_step();
  sim_recount();
  if (sim_counts[EL_STONE] <= stone_before) {
    puts("***FAIL powder selftest lava quench\n");
    return 1;
  }
  /* Save and reload through the real filesystem, then compare the grid. */
  sim_demo_scene();
  unsigned char first = sim_cells[5000];
  int fd = open("/powder/selftest.pwd");
  if (fd < 0) {
    puts("***FAIL powder selftest save open\n");
    return 1;
  }
  unsigned char header[16];
  header[0] = (unsigned char)SAVE_MAGIC0;
  header[1] = (unsigned char)SAVE_MAGIC1;
  header[2] = (unsigned char)SIM_W;
  header[3] = (unsigned char)SIM_H;
  for (int i = 4; i < 16; i++) header[i] = 0;
  int ok = write_all(fd, header, 16) == 0 &&
           write_all(fd, sim_cells, SIM_CELLS) == 0;
  close(fd);
  if (!ok) {
    puts("***FAIL powder selftest save write\n");
    return 1;
  }
  sim_clear();
  fd = open_existing("/powder/selftest.pwd");
  if (fd < 0 || read_all(fd, header, 16) != 0 ||
      read_all(fd, sim_cells, SIM_CELLS) != 0) {
    if (fd >= 0) close(fd);
    puts("***FAIL powder selftest load\n");
    return 1;
  }
  close(fd);
  if (sim_cells[5000] != first) {
    puts("***FAIL powder selftest load compare\n");
    return 1;
  }
  if (audio_open() == 0) {
    short tone[64];
    for (int i = 0; i < 64; i++) tone[i] = (i & 8) != 0 ? 3000 : -3000;
    if (audio_write(tone, 128) < 0 || audio_buffered() < 0 ||
        audio_close() != 0) {
      puts("***FAIL powder selftest audio\n");
      return 1;
    }
  }
  puts("***PASS powder guest engine\n");
  return 0;
}

/* Timed bench on a fixed scene; results are guest jiffies (3000 per second). */
static int run_bench(void) {
  if (setup_display() != 0) return 1;
  sim_init(20261012u);
  sim_demo_scene();
  for (int i = 0; i < 30; i++) {
    sim_step();
    render_sim();
  }
  unsigned t0 = get_current_jiffies();
  for (int i = 0; i < 300; i++) sim_step();
  unsigned sim_jiffies = get_current_jiffies() - t0;
  t0 = get_current_jiffies();
  for (int i = 0; i < 300; i++) {
    sim_step();
    render_sim();
  }
  unsigned both_jiffies = get_current_jiffies() - t0;
  int args[1];
  puts("***BENCH powder sim-only jiffies=");
  args[0] = (int)sim_jiffies;
  printf("%d\n", args);
  puts("***BENCH powder sim+render jiffies=");
  args[0] = (int)both_jiffies;
  printf("%d\n", args);
  /* Show the same numbers on screen until Q, for headless screenshots. */
  raw_mode = set_keyboard_raw(1) == 0;
  clear_chrome_boxes();
  draw_text(10, 24, "POWDER BENCH (300 STEPS, JIFFIES AT 3000/S)", COL_TITLE);
  draw_text(10, 26, "SIM ONLY:", COL_TEXT);
  draw_unsigned(24, 26, sim_jiffies, 8, COL_GOOD);
  draw_text(10, 27, "SIM+RENDER:", COL_TEXT);
  draw_unsigned(24, 27, both_jiffies, 8, COL_GOOD);
  draw_text(10, 29, "Q TO EXIT", COL_DIM);
  unsigned frame = get_vga_frame_counter();
  while (1) {
    frame = wait_frame(frame);
    poll_keyboard();
    if (key_pressed('q') || key_pressed('Q')) break;
    snapshot_keys();
  }
  if (raw_mode) set_keyboard_raw(0);
  return 0;
}

/* Entry point: --selftest checks, --demo skips the title, --silent mutes. */
int main(int argc, char** argv) {
  int selftest = 0;
  int demo = 0;
  int bench = 0;
  for (int i = 1; i < argc; i++) {
    if (streq(argv[i], "--selftest")) selftest = 1;
    if (streq(argv[i], "--silent")) sound_on = 0;
    if (streq(argv[i], "--demo")) demo = 1;
    if (streq(argv[i], "--bench")) bench = 1;
  }
  if (selftest) return run_selftest();
  if (bench) return run_bench();
  if (setup_display() != 0) return 1;

  sim_init(get_current_jiffies() | 1u);
  sim_demo_scene();
  render_invalidate();
  while (getmouse() != 0) {
    /* Discard stale events so the first click is really the user's. */
  }
  raw_mode = set_keyboard_raw(1) == 0;
  if (sound_on) audio_owned = audio_open() == 0;
  draw_cursor_sprite();
  if (demo) {
    state = ST_PLAY;
    attract = 1;
    clear_chrome_boxes();
  }
  draw_chrome();

  int quit = 0;
  int prev_fire = 0;
  int prev_stone = 0;
  unsigned frame = get_vga_frame_counter();
  unsigned stepped_frame = frame;
  while (!quit) {
    frame = wait_frame(frame);
    poll_mouse();
    poll_keyboard();
    quit = handle_input();
    if (!paused && state != ST_HELP) {
      if (frame - stepped_frame >= 1) {
        stepped_frame = frame;
        if (state == ST_TITLE || attract) attract_step();
        sim_step();
        if (sim_counts[EL_FIRE] > prev_fire + 4) sfx_start(1, 2500);
        if (sim_counts[EL_STONE] > prev_stone + 4) sfx_start(2, 2000);
        prev_fire = sim_counts[EL_FIRE];
        prev_stone = sim_counts[EL_STONE];
        if ((sim_steps & 15) == 0) {
          sim_recount();
          hud_dirty = 1;
        }
        render_sim();
      }
    }
    audio_pump();
    int sx = cursor_sx - 16;
    int sy = cursor_sy - 16;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    set_sprite_coords(0, (unsigned)sx, (unsigned)sy);
    if (hud_dirty) draw_chrome();
    if (sim_steps >= message_until && message_until != 0) {
      message_until = 0;
      hud_dirty = 1;
    }
    snapshot_keys();
  }

  if (raw_mode) set_keyboard_raw(0);
  if (audio_owned) audio_close();
  set_sprite_coords(0, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  return 0;
}


