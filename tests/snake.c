/*
 * Snake kernel test.
 *
 * Validates:
 * - headless runs exercise deterministic snake-state updates including growth,
 *   reverse-turn rejection, self-collision, and wall collisions
 * - VGA runs provide an interactive snake game driven by the PS/2 queue and
 *   tile framebuffer helpers
 * - interactive VGA runs load and update one SD1-backed ext2 high-score file
 *
 * How:
 * - keep all game state in fixed-size arrays sized to the board so the test
 *   does not depend on dynamic allocation during gameplay
 * - share one movement/collision engine between the headless smoke test and
 *   the interactive VGA loop
 * - in VGA mode, install a tiny custom snake/apple tileset, poll PS/2 events,
 *   redraw only when state changes, advance after a small frame interval, and
 *   persist new records to a fixed-width decimal file in the mounted ext2 root
 */

#include "../kernel/constants.h"
#include "../kernel/config.h"
#include "../kernel/debug.h"
#include "../kernel/ext.h"
#include "../kernel/pit.h"
#include "../kernel/print.h"
#include "../kernel/ps2.h"
#include "../kernel/threads.h"
#include "../kernel/vga.h"

#define BOARD_WIDTH 30
#define BOARD_HEIGHT 20
#define BOARD_CELLS 600

#define INITIAL_LENGTH 4

#define HUD_LEFT 1

#define BOARD_LEFT 4
#define BOARD_TOP 8
#define BOARD_RIGHT 35
#define BOARD_BOTTOM 29

#define TITLE_ROW 1
#define SCORE_ROW 3
#define HELP_ROW 4
#define STATUS_ROW 6

#define MOVE_FRAME_INTERVAL 2

#define RNG_MULTIPLIER 1664525u
#define RNG_INCREMENT 1013904223u
#define INITIAL_RNG_STATE 0x534E414Bu
#define HIGH_SCORE_FILE_NAME "high_score.txt"
#define HIGH_SCORE_FILE_DIGITS 10
#define HIGH_SCORE_FILE_BYTES 11

#define BORDER_COLOR 0xFC
#define TITLE_COLOR 0xFE
#define TEXT_COLOR 0x92
#define STATUS_COLOR 0x92

#define FLOOR_TILE 128
#define APPLE_TILE 129
#define HEAD_UP_TILE 130
#define HEAD_RIGHT_TILE 131
#define HEAD_DOWN_TILE 132
#define HEAD_LEFT_TILE 133
#define BODY_HORIZONTAL_TILE 134
#define BODY_VERTICAL_TILE 135
#define TURN_UP_RIGHT_TILE 136
#define TURN_RIGHT_DOWN_TILE 137
#define TURN_DOWN_LEFT_TILE 138
#define TURN_LEFT_UP_TILE 139
#define TAIL_UP_TILE 140
#define TAIL_RIGHT_TILE 141
#define TAIL_DOWN_TILE 142
#define TAIL_LEFT_TILE 143

// 12-bit VGA pixels use red in bits [3:0], green in [7:4], and blue in [11:8],
// so these packed constants follow the documented `0x0BGR` nibble order.
#define FLOOR_DARK 0x123
#define FLOOR_LIGHT 0x345
#define FLOOR_SPARK 0x567

#define SNAKE_OUTLINE 0x140
#define SNAKE_BODY 0x3C3
#define SNAKE_HIGHLIGHT 0x7F6
#define EYE_WHITE 0xEEE
#define EYE_PUPIL 0x111

#define APPLE_SHADOW 0x005
#define APPLE_RED 0x01D
#define APPLE_HIGHLIGHT 0x55E
#define STEM_BROWN 0x146
#define LEAF_GREEN 0x4D3

#define KEY_RELEASE_MASK 0xFF00
#define KEY_ASCII_MASK 0x00FF

#define KEY_UP 'w'
#define KEY_UP_ALT 'W'
#define KEY_LEFT 'a'
#define KEY_LEFT_ALT 'A'
#define KEY_DOWN 's'
#define KEY_DOWN_ALT 'S'
#define KEY_RIGHT 'd'
#define KEY_RIGHT_ALT 'D'
#define KEY_PAUSE 'p'
#define KEY_PAUSE_ALT 'P'
#define KEY_RESTART 'r'
#define KEY_RESTART_ALT 'R'
#define KEY_QUIT 'q'
#define KEY_QUIT_ALT 'Q'

enum Direction {
  DIR_UP = 0,
  DIR_RIGHT = 1,
  DIR_DOWN = 2,
  DIR_LEFT = 3,
};

struct SnakeGame {
  int snake_x[BOARD_CELLS];
  int snake_y[BOARD_CELLS];
  char occupied[BOARD_CELLS];
  int tail_slot;
  int head_slot;
  int length;
  int direction;
  int queued_direction;
  int food_x;
  int food_y;
  unsigned rng_state;
  int score;
  bool alive;
  bool won;
};

static struct SnakeGame game;
static struct Ext2 snake_fs;
static unsigned snake_high_score = 0;
static unsigned snake_saved_high_score = 0;
static bool snake_high_score_dirty = false;
static bool snake_fs_ready = false;
static unsigned* vga_frame_counter = (unsigned*)0x7FE5B48;
// docs/mem_map.md defines VGA status at the single byte address 0x7FE5B46.
// Use a byte-wide MMIO read here; widening to 16 bits touches 0x7FE5B47,
// which is not part of the documented register and crashes the emulator.
static char* vga_status = (char*)0x7FE5B46;
static bool poll_input(struct SnakeGame* state, bool* paused,
                       bool* restart_requested, bool* quit_requested);

// Write one tile cell directly into the text framebuffer.
static void put_tile_at(int x, int y, int tile, int color) {
  TILE_FB[y * TILE_ROW_WIDTH + x] = (short)((color << 8) | (tile & 0xFF));
}

// Flatten one tile-local (x, y) coordinate into the tilemap slot index.
static int tile_pixel_index(int tile, int x, int y) {
  return tile * 64 + y * 8 + x;
}

// Fill one 8x8 tile with a fixed pixel value.
static void fill_tile_pixels(int tile, short pixel) {
  int base = tile * 64;
  for (int i = 0; i < 64; i++) {
    TILEMAP[base + i] = pixel;
  }
}

// Copy the full contents of one tile bitmap into another tile slot.
static void copy_tile_pixels(int dst_tile, int src_tile) {
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      TILEMAP[tile_pixel_index(dst_tile, x, y)] =
        TILEMAP[tile_pixel_index(src_tile, x, y)];
    }
  }
}

// Write one fixed-color pixel inside one tile bitmap.
static void set_tile_pixel(int tile, int x, int y, short pixel) {
  TILEMAP[tile_pixel_index(tile, x, y)] = pixel;
}

// Fill an inclusive rectangle inside one tile bitmap.
static void fill_tile_rect(int tile, int x0, int y0, int x1, int y1, short pixel) {
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      set_tile_pixel(tile, x, y, pixel);
    }
  }
}

// Paint a few floor speckles so empty cells are not flat blocks of color.
static void install_floor_tile(void) {
  fill_tile_pixels(FLOOR_TILE, FLOOR_DARK);

  fill_tile_rect(FLOOR_TILE, 1, 1, 2, 2, FLOOR_LIGHT);
  fill_tile_rect(FLOOR_TILE, 5, 1, 6, 2, FLOOR_LIGHT);
  fill_tile_rect(FLOOR_TILE, 2, 5, 3, 6, FLOOR_LIGHT);
  fill_tile_rect(FLOOR_TILE, 5, 5, 5, 5, FLOOR_LIGHT);

  set_tile_pixel(FLOOR_TILE, 3, 1, FLOOR_SPARK);
  set_tile_pixel(FLOOR_TILE, 6, 4, FLOOR_SPARK);
  set_tile_pixel(FLOOR_TILE, 1, 6, FLOOR_SPARK);
}

// Build one body/corner tile by connecting the tile center to selected edges.
static void install_segment_tile(int tile, bool up, bool right, bool down, bool left) {
  copy_tile_pixels(tile, FLOOR_TILE);

  fill_tile_rect(tile, 2, 2, 5, 5, SNAKE_OUTLINE);
  fill_tile_rect(tile, 3, 3, 4, 4, SNAKE_HIGHLIGHT);

  if (up) {
    fill_tile_rect(tile, 2, 0, 5, 3, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 0, 4, 2, SNAKE_HIGHLIGHT);
  }

  if (right) {
    fill_tile_rect(tile, 4, 2, 7, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 5, 3, 7, 4, SNAKE_HIGHLIGHT);
  }

  if (down) {
    fill_tile_rect(tile, 2, 4, 5, 7, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 5, 4, 7, SNAKE_HIGHLIGHT);
  }

  if (left) {
    fill_tile_rect(tile, 0, 2, 3, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 0, 3, 2, 4, SNAKE_HIGHLIGHT);
  }

  fill_tile_rect(tile, 2, 2, 5, 5, SNAKE_BODY);

  if (up) fill_tile_rect(tile, 2, 0, 5, 3, SNAKE_BODY);
  if (right) fill_tile_rect(tile, 4, 2, 7, 5, SNAKE_BODY);
  if (down) fill_tile_rect(tile, 2, 4, 5, 7, SNAKE_BODY);
  if (left) fill_tile_rect(tile, 0, 2, 3, 5, SNAKE_BODY);

  fill_tile_rect(tile, 3, 1, 4, 1, SNAKE_HIGHLIGHT);
  fill_tile_rect(tile, 2, 2, 2, 4, SNAKE_HIGHLIGHT);
}

// Build one tail tile that tapers toward the non-connected edge.
static void install_tail_tile(int tile, int direction) {
  copy_tile_pixels(tile, FLOOR_TILE);

  if (direction == DIR_UP) {
    fill_tile_rect(tile, 2, 0, 5, 7, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 0, 4, 7, SNAKE_BODY);
    fill_tile_rect(tile, 3, 0, 4, 1, SNAKE_HIGHLIGHT);
  } else if (direction == DIR_RIGHT) {
    fill_tile_rect(tile, 0, 2, 7, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 0, 3, 7, 4, SNAKE_BODY);
    fill_tile_rect(tile, 6, 3, 7, 4, SNAKE_HIGHLIGHT);
  } else if (direction == DIR_DOWN) {
    fill_tile_rect(tile, 2, 0, 5, 7, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 0, 4, 7, SNAKE_BODY);
    fill_tile_rect(tile, 3, 6, 4, 7, SNAKE_HIGHLIGHT);
  } else {
    fill_tile_rect(tile, 0, 2, 7, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 0, 3, 7, 4, SNAKE_BODY);
    fill_tile_rect(tile, 0, 3, 1, 4, SNAKE_HIGHLIGHT);
  }
}

// Build one head tile with simple eye detail pointing in the travel direction.
static void install_head_tile(int tile, int direction) {
  copy_tile_pixels(tile, FLOOR_TILE);

  if (direction == DIR_UP) {
    fill_tile_rect(tile, 2, 0, 5, 6, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 0, 4, 6, SNAKE_BODY);
    fill_tile_rect(tile, 2, 1, 5, 2, SNAKE_BODY);
    set_tile_pixel(tile, 2, 2, EYE_WHITE);
    set_tile_pixel(tile, 5, 2, EYE_WHITE);
    set_tile_pixel(tile, 2, 1, EYE_PUPIL);
    set_tile_pixel(tile, 5, 1, EYE_PUPIL);
    fill_tile_rect(tile, 3, 0, 4, 1, SNAKE_HIGHLIGHT);
  } else if (direction == DIR_RIGHT) {
    fill_tile_rect(tile, 1, 2, 7, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 1, 3, 7, 4, SNAKE_BODY);
    fill_tile_rect(tile, 5, 2, 7, 5, SNAKE_BODY);
    set_tile_pixel(tile, 5, 2, EYE_WHITE);
    set_tile_pixel(tile, 5, 5, EYE_WHITE);
    set_tile_pixel(tile, 6, 2, EYE_PUPIL);
    set_tile_pixel(tile, 6, 5, EYE_PUPIL);
    fill_tile_rect(tile, 6, 3, 7, 4, SNAKE_HIGHLIGHT);
  } else if (direction == DIR_DOWN) {
    fill_tile_rect(tile, 2, 1, 5, 7, SNAKE_OUTLINE);
    fill_tile_rect(tile, 3, 1, 4, 7, SNAKE_BODY);
    fill_tile_rect(tile, 2, 5, 5, 7, SNAKE_BODY);
    set_tile_pixel(tile, 2, 5, EYE_WHITE);
    set_tile_pixel(tile, 5, 5, EYE_WHITE);
    set_tile_pixel(tile, 2, 6, EYE_PUPIL);
    set_tile_pixel(tile, 5, 6, EYE_PUPIL);
    fill_tile_rect(tile, 3, 6, 4, 7, SNAKE_HIGHLIGHT);
  } else {
    fill_tile_rect(tile, 0, 2, 6, 5, SNAKE_OUTLINE);
    fill_tile_rect(tile, 0, 3, 6, 4, SNAKE_BODY);
    fill_tile_rect(tile, 0, 2, 2, 5, SNAKE_BODY);
    set_tile_pixel(tile, 2, 2, EYE_WHITE);
    set_tile_pixel(tile, 2, 5, EYE_WHITE);
    set_tile_pixel(tile, 1, 2, EYE_PUPIL);
    set_tile_pixel(tile, 1, 5, EYE_PUPIL);
    fill_tile_rect(tile, 0, 3, 1, 4, SNAKE_HIGHLIGHT);
  }
}

// Build one opaque apple tile so it sits naturally on top of the board floor.
static void install_apple_tile(void) {
  copy_tile_pixels(APPLE_TILE, FLOOR_TILE);

  set_tile_pixel(APPLE_TILE, 4, 0, STEM_BROWN);
  set_tile_pixel(APPLE_TILE, 3, 1, STEM_BROWN);
  set_tile_pixel(APPLE_TILE, 5, 1, LEAF_GREEN);
  set_tile_pixel(APPLE_TILE, 6, 1, LEAF_GREEN);

  fill_tile_rect(APPLE_TILE, 2, 2, 5, 6, APPLE_SHADOW);
  fill_tile_rect(APPLE_TILE, 1, 3, 6, 6, APPLE_RED);
  fill_tile_rect(APPLE_TILE, 2, 2, 5, 5, APPLE_RED);
  fill_tile_rect(APPLE_TILE, 3, 4, 4, 6, APPLE_RED);
  fill_tile_rect(APPLE_TILE, 2, 2, 3, 3, APPLE_HIGHLIGHT);
  set_tile_pixel(APPLE_TILE, 4, 3, APPLE_HIGHLIGHT);
  set_tile_pixel(APPLE_TILE, 4, 2, APPLE_HIGHLIGHT);
}

// Install the snake-specific 8x8 tiles into unused tile indices after the font loads.
static void install_game_tiles(void) {
  install_floor_tile();
  install_apple_tile();

  install_head_tile(HEAD_UP_TILE, DIR_UP);
  install_head_tile(HEAD_RIGHT_TILE, DIR_RIGHT);
  install_head_tile(HEAD_DOWN_TILE, DIR_DOWN);
  install_head_tile(HEAD_LEFT_TILE, DIR_LEFT);

  install_segment_tile(BODY_HORIZONTAL_TILE, false, true, false, true);
  install_segment_tile(BODY_VERTICAL_TILE, true, false, true, false);
  install_segment_tile(TURN_UP_RIGHT_TILE, true, true, false, false);
  install_segment_tile(TURN_RIGHT_DOWN_TILE, false, true, true, false);
  install_segment_tile(TURN_DOWN_LEFT_TILE, false, false, true, true);
  install_segment_tile(TURN_LEFT_UP_TILE, true, false, false, true);

  install_tail_tile(TAIL_UP_TILE, DIR_UP);
  install_tail_tile(TAIL_RIGHT_TILE, DIR_RIGHT);
  install_tail_tile(TAIL_DOWN_TILE, DIR_DOWN);
  install_tail_tile(TAIL_LEFT_TILE, DIR_LEFT);
}

// Clear one text row before drawing HUD text on it.
static void clear_row(int y) {
  for (int x = 0; x < TILE_ROW_WIDTH; x++) {
    put_tile_at(x, y, ' ', TITLE_COLOR);
  }
}

// Draw one NUL-terminated string at a fixed tile position.
static void draw_text(int x, int y, char* text, int color) {
  while (*text != '\0') {
    put_tile_at(x, y, *text, color);
    x++;
    text++;
  }
}

// Draw an unsigned decimal value right-aligned inside a fixed-width field.
static void draw_unsigned_fixed(int x, int y, unsigned value, int width, int color) {
  for (int i = 0; i < width; i++) {
    put_tile_at(x + i, y, ' ', color);
  }

  int pos = x + width - 1;
  do {
    put_tile_at(pos, y, (char)('0' + (value % 10)), color);
    value /= 10;
    pos--;
  } while (value != 0 && pos >= x);
}

// Encode one fixed-width decimal record so the high-score file can be
// overwritten in place without needing truncate support.
static void encode_high_score_record(unsigned score, char* dest) {
  for (int i = HIGH_SCORE_FILE_DIGITS - 1; i >= 0; i--) {
    dest[i] = (char)('0' + (score % 10));
    score /= 10;
  }

  dest[HIGH_SCORE_FILE_DIGITS] = '\n';
}

// Decode one on-disk high-score record. The file format is intentionally fixed
// width so any rewrite can reuse the same ext2 byte range.
static bool decode_high_score_record(char* src, unsigned size, unsigned* score) {
  unsigned value = 0;

  if (size < HIGH_SCORE_FILE_BYTES) return false;

  for (unsigned i = 0; i < HIGH_SCORE_FILE_DIGITS; i++) {
    char digit = src[i];
    if (digit < '0' || digit > '9') return false;
    value = value * 10 + (unsigned)(digit - '0');
  }

  if (src[HIGH_SCORE_FILE_DIGITS] != '\n') return false;

  *score = value;
  return true;
}

// Open the root high-score file, creating and seeding it on first use.
static struct Node* open_high_score_file(struct Ext2* fs) {
  struct Node* file = node_find(&fs->root, HIGH_SCORE_FILE_NAME);
  char score_buf[HIGH_SCORE_FILE_BYTES];
  unsigned wrote;

  if (file != NULL) return file;

  file = node_make_file(&fs->root, HIGH_SCORE_FILE_NAME);
  assert(file != NULL,
         "snake: failed to create high score file in filesystem root.\n");

  encode_high_score_record(0, score_buf);
  wrote = node_write_all(file, 0, HIGH_SCORE_FILE_BYTES, score_buf);
  assert(wrote == HIGH_SCORE_FILE_BYTES,
         "snake: failed to seed high score file contents.\n");
  return file;
}

// Load the persisted high score from the SD1 ext2 image, repairing the file to
// a canonical zero record if the contents are missing or malformed.
static unsigned load_high_score(struct Ext2* fs) {
  struct Node* file = open_high_score_file(fs);
  char score_buf[HIGH_SCORE_FILE_BYTES];
  unsigned high_score = 0;
  unsigned bytes_read = node_read_all(file, 0, HIGH_SCORE_FILE_BYTES, score_buf);

  if (!decode_high_score_record(score_buf, bytes_read, &high_score)) {
    encode_high_score_record(0, score_buf);
    unsigned wrote = node_write_all(file, 0, HIGH_SCORE_FILE_BYTES, score_buf);
    assert(wrote == HIGH_SCORE_FILE_BYTES,
           "snake: failed to repair malformed high score file.\n");
    high_score = 0;
  }

  node_free(file);
  return high_score;
}

// Persist the new record immediately so quitting after a fresh high score does
// not lose it.
static void save_high_score(struct Ext2* fs, unsigned high_score) {
  struct Node* file = open_high_score_file(fs);
  char score_buf[HIGH_SCORE_FILE_BYTES];
  encode_high_score_record(high_score, score_buf);
  unsigned wrote = node_write_all(file, 0, HIGH_SCORE_FILE_BYTES, score_buf);
  assert(wrote == HIGH_SCORE_FILE_BYTES,
         "snake: failed to save updated high score.\n");
  node_free(file);
}

// Mount the SD1 ext2 image for the interactive test and cache the saved score
// until the game exits back to the harness.
static void mount_high_score_fs(void) {
  ext2_init(&snake_fs);
  snake_saved_high_score = load_high_score(&snake_fs);
  snake_high_score = snake_saved_high_score;
  snake_high_score_dirty = false;
  snake_fs_ready = true;
}

// Release every ext2 allocation after the interactive game finishes so the
// test returns with no live filesystem wrappers or caches.
static void unmount_high_score_fs(void) {
  if (!snake_fs_ready) return;
  ext2_destroy(&snake_fs);
  snake_fs_ready = false;
}

// Track one in-memory record candidate for the current round without writing it
// back until the round actually ends.
static void maybe_record_high_score(int score) {
  unsigned candidate = (unsigned)score;

  if (candidate <= snake_high_score) return;

  snake_high_score = candidate;
  snake_high_score_dirty = true;
}

// Commit one pending record only after the round ends in a terminal state.
static void commit_high_score(void) {
  if (!snake_high_score_dirty) return;

  if (snake_fs_ready) {
    save_high_score(&snake_fs, snake_high_score);
  }

  snake_saved_high_score = snake_high_score;
  snake_high_score_dirty = false;
}

// Flatten one (x, y) board coordinate into the occupancy array index.
static int board_index(int x, int y) {
  return y * BOARD_WIDTH + x;
}

// Reset the board occupancy grid before repopulating it.
static void clear_occupied(struct SnakeGame* state) {
  for (int i = 0; i < BOARD_CELLS; i++) {
    state->occupied[i] = 0;
  }
}

// Advance one ring-buffer slot and wrap back to slot zero.
static int next_slot(int slot) {
  slot++;
  if (slot == BOARD_CELLS) return 0;
  return slot;
}

// Return the x-axis delta for one direction.
static int direction_dx(int direction) {
  if (direction == DIR_RIGHT) return 1;
  if (direction == DIR_LEFT) return -1;
  return 0;
}

// Return the y-axis delta for one direction.
static int direction_dy(int direction) {
  if (direction == DIR_DOWN) return 1;
  if (direction == DIR_UP) return -1;
  return 0;
}

// Reverse turns are illegal because they would move the head back into the neck.
static bool directions_are_opposites(int a, int b) {
  if (a == DIR_UP && b == DIR_DOWN) return true;
  if (a == DIR_DOWN && b == DIR_UP) return true;
  if (a == DIR_LEFT && b == DIR_RIGHT) return true;
  if (a == DIR_RIGHT && b == DIR_LEFT) return true;
  return false;
}

// Convert WASD input into one movement direction.
static int direction_from_key(int key) {
  if (key == KEY_UP || key == KEY_UP_ALT) return DIR_UP;
  if (key == KEY_RIGHT || key == KEY_RIGHT_ALT) return DIR_RIGHT;
  if (key == KEY_DOWN || key == KEY_DOWN_ALT) return DIR_DOWN;
  if (key == KEY_LEFT || key == KEY_LEFT_ALT) return DIR_LEFT;
  return -1;
}

// Queue the next turn, rejecting only immediate reversals of the current heading.
static void queue_direction(struct SnakeGame* state, int direction) {
  if (direction < 0) return;
  if (directions_are_opposites(state->direction, direction)) return;
  state->queued_direction = direction;
}

// Advance the deterministic food-placement PRNG.
static unsigned next_random(struct SnakeGame* state) {
  state->rng_state = state->rng_state * RNG_MULTIPLIER + RNG_INCREMENT;
  return state->rng_state;
}

// Place food on the first free cell at or after the pseudo-random probe point.
static void place_food(struct SnakeGame* state) {
  unsigned start = next_random(state) % BOARD_CELLS;

  for (int offset = 0; offset < BOARD_CELLS; offset++) {
    int idx = (int)((start + offset) % BOARD_CELLS);
    int x = idx % BOARD_WIDTH;
    int y = idx / BOARD_WIDTH;

    if (!state->occupied[board_index(x, y)]) {
      state->food_x = x;
      state->food_y = y;
      return;
    }
  }

  panic("snake: failed to place food on a non-full board\n");
}

// Force food onto a known free cell for deterministic smoke-test scenarios.
static void set_food(struct SnakeGame* state, int x, int y) {
  assert(x >= 0 && x < BOARD_WIDTH, "snake smoke: food x out of range.\n");
  assert(y >= 0 && y < BOARD_HEIGHT, "snake smoke: food y out of range.\n");
  assert(!state->occupied[board_index(x, y)],
         "snake smoke: food placed on snake body.\n");
  state->food_x = x;
  state->food_y = y;
}

// Reset the game to a centered horizontal snake and one deterministic food item.
static void reset_game(struct SnakeGame* state, unsigned seed) {
  int start_x = (BOARD_WIDTH / 2) - (INITIAL_LENGTH / 2);
  int start_y = BOARD_HEIGHT / 2;

  clear_occupied(state);

  state->tail_slot = 0;
  state->head_slot = INITIAL_LENGTH - 1;
  state->length = INITIAL_LENGTH;
  state->direction = DIR_RIGHT;
  state->queued_direction = DIR_RIGHT;
  state->food_x = -1;
  state->food_y = -1;
  state->rng_state = seed;
  state->score = 0;
  state->alive = true;
  state->won = false;

  for (int i = 0; i < INITIAL_LENGTH; i++) {
    int x = start_x + i;
    state->snake_x[i] = x;
    state->snake_y[i] = start_y;
    state->occupied[board_index(x, start_y)] = 1;
  }

  place_food(state);
}

// Build a compact U shape that will self-collide on the next upward move.
static void load_self_collision_shape(struct SnakeGame* state) {
  clear_occupied(state);

  state->tail_slot = 0;
  state->head_slot = 4;
  state->length = 5;
  state->direction = DIR_UP;
  state->queued_direction = DIR_UP;
  state->food_x = 0;
  state->food_y = 0;
  state->rng_state = INITIAL_RNG_STATE;
  state->score = 2;
  state->alive = true;
  state->won = false;

  state->snake_x[0] = 5;
  state->snake_y[0] = 5;
  state->snake_x[1] = 6;
  state->snake_y[1] = 5;
  state->snake_x[2] = 7;
  state->snake_y[2] = 5;
  state->snake_x[3] = 7;
  state->snake_y[3] = 6;
  state->snake_x[4] = 6;
  state->snake_y[4] = 6;

  for (int i = 0; i < state->length; i++) {
    state->occupied[board_index(state->snake_x[i], state->snake_y[i])] = 1;
  }
}

// Step the snake once, handling growth, wall hits, tail movement, and collisions.
static void advance_game(struct SnakeGame* state) {
  int head_x;
  int head_y;
  int next_x;
  int next_y;
  int tail_x;
  int tail_y;
  bool grow;

  if (!state->alive || state->won) return;

  state->direction = state->queued_direction;

  head_x = state->snake_x[state->head_slot];
  head_y = state->snake_y[state->head_slot];
  next_x = head_x + direction_dx(state->direction);
  next_y = head_y + direction_dy(state->direction);

  if (next_x < 0 || next_x >= BOARD_WIDTH ||
      next_y < 0 || next_y >= BOARD_HEIGHT) {
    state->alive = false;
    return;
  }

  grow = (next_x == state->food_x && next_y == state->food_y);
  tail_x = state->snake_x[state->tail_slot];
  tail_y = state->snake_y[state->tail_slot];

  if (state->occupied[board_index(next_x, next_y)] &&
      (grow || next_x != tail_x || next_y != tail_y)) {
    state->alive = false;
    return;
  }

  if (!grow) {
    state->occupied[board_index(tail_x, tail_y)] = 0;
    state->tail_slot = next_slot(state->tail_slot);
  }

  state->head_slot = next_slot(state->head_slot);
  state->snake_x[state->head_slot] = next_x;
  state->snake_y[state->head_slot] = next_y;
  state->occupied[board_index(next_x, next_y)] = 1;

  if (grow) {
    state->length++;
    state->score++;

    if (state->length == BOARD_CELLS) {
      state->won = true;
      state->food_x = -1;
      state->food_y = -1;
      return;
    }

    place_food(state);
  }
}

// Return the ring-buffer slot for one logical snake segment index.
static int snake_slot_at(struct SnakeGame* state, int order) {
  int slot = state->tail_slot + order;
  if (slot >= BOARD_CELLS) slot -= BOARD_CELLS;
  return slot;
}

// Convert one adjacent-cell delta into a cardinal direction.
static int direction_between(int from_x, int from_y, int to_x, int to_y) {
  if (to_x == from_x && to_y == from_y - 1) return DIR_UP;
  if (to_x == from_x + 1 && to_y == from_y) return DIR_RIGHT;
  if (to_x == from_x && to_y == from_y + 1) return DIR_DOWN;
  if (to_x == from_x - 1 && to_y == from_y) return DIR_LEFT;
  panic("snake: non-adjacent body segment in tile selection\n");
  return DIR_UP;
}

// Map one tail connection direction to the matching tail tile.
static int tail_tile_for_direction(int direction) {
  if (direction == DIR_UP) return TAIL_UP_TILE;
  if (direction == DIR_RIGHT) return TAIL_RIGHT_TILE;
  if (direction == DIR_DOWN) return TAIL_DOWN_TILE;
  return TAIL_LEFT_TILE;
}

// Map one heading direction to the matching head tile.
static int head_tile_for_direction(int direction) {
  if (direction == DIR_UP) return HEAD_UP_TILE;
  if (direction == DIR_RIGHT) return HEAD_RIGHT_TILE;
  if (direction == DIR_DOWN) return HEAD_DOWN_TILE;
  return HEAD_LEFT_TILE;
}

// Choose the correct tile for one snake segment based on its neighbors.
static int tile_for_segment(struct SnakeGame* state, int order) {
  int slot = snake_slot_at(state, order);
  int cur_x = state->snake_x[slot];
  int cur_y = state->snake_y[slot];

  if (order == state->length - 1) {
    return head_tile_for_direction(state->direction);
  }

  if (order == 0) {
    int next_slot_index = snake_slot_at(state, 1);
    int next_x = state->snake_x[next_slot_index];
    int next_y = state->snake_y[next_slot_index];
    int direction = direction_between(cur_x, cur_y, next_x, next_y);
    return tail_tile_for_direction(direction);
  }

  int prev_slot = snake_slot_at(state, order - 1);
  int next_slot_index = snake_slot_at(state, order + 1);
  int prev_x = state->snake_x[prev_slot];
  int prev_y = state->snake_y[prev_slot];
  int next_x = state->snake_x[next_slot_index];
  int next_y = state->snake_y[next_slot_index];
  bool up = (prev_y == cur_y - 1) || (next_y == cur_y - 1);
  bool right = (prev_x == cur_x + 1) || (next_x == cur_x + 1);
  bool down = (prev_y == cur_y + 1) || (next_y == cur_y + 1);
  bool left = (prev_x == cur_x - 1) || (next_x == cur_x - 1);

  if (left && right) return BODY_HORIZONTAL_TILE;
  if (up && down) return BODY_VERTICAL_TILE;
  if (up && right) return TURN_UP_RIGHT_TILE;
  if (right && down) return TURN_RIGHT_DOWN_TILE;
  if (down && left) return TURN_DOWN_LEFT_TILE;
  return TURN_LEFT_UP_TILE;
}

// Draw one logical board cell as one native tile entry.
static void draw_cell(int x, int y, int tile, int color) {
  int tile_x = BOARD_LEFT + 1 + x;
  int tile_y = BOARD_TOP + 1 + y;

  put_tile_at(tile_x, tile_y, tile, color);
}

// Draw the rectangular border around the snake board.
static void draw_border(void) {
  put_tile_at(BOARD_LEFT, BOARD_TOP, '+', BORDER_COLOR);
  put_tile_at(BOARD_RIGHT, BOARD_TOP, '+', BORDER_COLOR);
  put_tile_at(BOARD_LEFT, BOARD_BOTTOM, '+', BORDER_COLOR);
  put_tile_at(BOARD_RIGHT, BOARD_BOTTOM, '+', BORDER_COLOR);

  for (int x = BOARD_LEFT + 1; x < BOARD_RIGHT; x++) {
    put_tile_at(x, BOARD_TOP, '-', BORDER_COLOR);
    put_tile_at(x, BOARD_BOTTOM, '-', BORDER_COLOR);
  }

  for (int y = BOARD_TOP + 1; y < BOARD_BOTTOM; y++) {
    put_tile_at(BOARD_LEFT, y, '|', BORDER_COLOR);
    put_tile_at(BOARD_RIGHT, y, '|', BORDER_COLOR);
  }
}

// Draw the title, current score, best score, controls, and state message above
// the board.
static void draw_hud(struct SnakeGame* state, bool paused) {
  clear_row(TITLE_ROW);
  clear_row(HELP_ROW);
  clear_row(STATUS_ROW);

  draw_text(HUD_LEFT, TITLE_ROW, "SNAKE", TITLE_COLOR);

  clear_row(SCORE_ROW);
  draw_text(HUD_LEFT, SCORE_ROW, "Score:", TEXT_COLOR);
  draw_unsigned_fixed(HUD_LEFT + 7, SCORE_ROW, (unsigned)state->score, 3, TEXT_COLOR);
  draw_text(HUD_LEFT + 15, SCORE_ROW, "Best:", TEXT_COLOR);
  draw_unsigned_fixed(HUD_LEFT + 21, SCORE_ROW, snake_high_score, 3, TEXT_COLOR);

  draw_text(HUD_LEFT, HELP_ROW,
            "WASD move  P pause  R reset", TEXT_COLOR);

  if (state->won) {
    draw_text(HUD_LEFT, STATUS_ROW,
              "You win. R reset  Q quit",
              STATUS_COLOR);
  } else if (!state->alive) {
    draw_text(HUD_LEFT, STATUS_ROW,
              "Game over. R reset  Q quit",
              STATUS_COLOR);
  } else if (paused) {
    draw_text(HUD_LEFT, STATUS_ROW,
              "Paused. P resume  R reset  Q quit",
              STATUS_COLOR);
  } else {
    draw_text(HUD_LEFT, STATUS_ROW,
              "Eat the red apple.",
              STATUS_COLOR);
  }
}

// Update only the score row after growth or a new record.
static void redraw_score_row(struct SnakeGame* state) {
  clear_row(SCORE_ROW);
  draw_text(HUD_LEFT, SCORE_ROW, "Score:", TEXT_COLOR);
  draw_unsigned_fixed(HUD_LEFT + 7, SCORE_ROW, (unsigned)state->score, 3, TEXT_COLOR);
  draw_text(HUD_LEFT + 15, SCORE_ROW, "Best:", TEXT_COLOR);
  draw_unsigned_fixed(HUD_LEFT + 21, SCORE_ROW, snake_high_score, 3, TEXT_COLOR);
}

// Wait until the display enters a new vblank interval before touching many tiles.
static void wait_for_vblank_start(void) {
  while ((*vga_status & 1) != 0) {
    // wait for the current vblank to finish so we can detect the next one
  }

  while ((*vga_status & 1) == 0) {
    // wait for the next vblank to begin
  }
}

// Redraw the full board from the current occupancy grid and food/head markers.
static void draw_board(struct SnakeGame* state) {
  draw_border();

  for (int y = 0; y < BOARD_HEIGHT; y++) {
    for (int x = 0; x < BOARD_WIDTH; x++) {
      draw_cell(x, y, FLOOR_TILE, 0);
    }
  }

  if (state->food_x >= 0 && state->food_y >= 0) {
    draw_cell(state->food_x, state->food_y, APPLE_TILE, 0);
  }

  for (int order = 0; order < state->length; order++) {
    int slot = snake_slot_at(state, order);
    draw_cell(state->snake_x[slot], state->snake_y[slot],
              tile_for_segment(state, order), 0);
  }
}

// Draw one already-classified snake segment by logical order.
static void draw_segment_order(struct SnakeGame* state, int order) {
  int slot = snake_slot_at(state, order);
  draw_cell(state->snake_x[slot], state->snake_y[slot],
            tile_for_segment(state, order), 0);
}

// Drain stale keypresses before a fresh interactive session begins.
static void drain_keys(void) {
  while (getkey() != 0) {
    // drop any buffered key events from a previous test or shutdown prompt
  }
}

// Poll until the VGA frame counter changes so the game advances once per frame.
static unsigned wait_for_next_frame(unsigned last_frame) {
  unsigned frame = last_frame;

  while (frame == last_frame) {
    frame = *vga_frame_counter;
  }

  return frame;
}

// Redraw the game board and HUD after a state change.
static void redraw(struct SnakeGame* state, bool paused) {
  wait_for_vblank_start();
  draw_hud(state, paused);
  draw_board(state);
}

// Update only the cells that changed during one successful non-growth move.
static void redraw_move_delta(struct SnakeGame* state,
                              int old_tail_x, int old_tail_y,
                              int old_head_x, int old_head_y) {
  wait_for_vblank_start();

  draw_cell(old_tail_x, old_tail_y, FLOOR_TILE, 0);

  if (state->length > 1) {
    draw_segment_order(state, 0);
    draw_cell(old_head_x, old_head_y,
              tile_for_segment(state, state->length - 2), 0);
  }

  draw_segment_order(state, state->length - 1);
}

// Update only the cells and HUD state affected by eating one apple.
static void redraw_growth_delta(struct SnakeGame* state,
                                int old_head_x, int old_head_y) {
  wait_for_vblank_start();

  if (state->length > 1) {
    draw_cell(old_head_x, old_head_y,
              tile_for_segment(state, state->length - 2), 0);
  }

  draw_segment_order(state, state->length - 1);

  if (state->food_x >= 0 && state->food_y >= 0) {
    draw_cell(state->food_x, state->food_y, APPLE_TILE, 0);
  }

  redraw_score_row(state);
}

// Run one game instance until the player restarts or quits.
static bool play_round(unsigned seed, unsigned* next_seed) {
  bool paused = false;
  bool restart_requested = false;
  bool quit_requested = false;
  bool dirty = true;
  unsigned last_frame = *vga_frame_counter;
  unsigned move_frames_remaining = MOVE_FRAME_INTERVAL;

  reset_game(&game, seed);
  snake_high_score = snake_saved_high_score;
  snake_high_score_dirty = false;
  *next_seed = seed * RNG_MULTIPLIER + RNG_INCREMENT + current_jiffies;

  while (!restart_requested && !quit_requested) {
    if (poll_input(&game, &paused, &restart_requested, &quit_requested)) {
      dirty = true;
    }

    if (dirty) {
      redraw(&game, paused);
      dirty = false;
    }

    if (quit_requested) return false;
    if (restart_requested) return true;

    if (paused || !game.alive || game.won) {
      continue;
    }

    last_frame = wait_for_next_frame(last_frame);

    if (poll_input(&game, &paused, &restart_requested, &quit_requested)) {
      dirty = true;
    }

    if (quit_requested) return false;
    if (restart_requested) return true;

    if (paused || !game.alive || game.won) {
      continue;
    }

    move_frames_remaining--;
    if (move_frames_remaining != 0) {
      continue;
    }
    move_frames_remaining = MOVE_FRAME_INTERVAL;

    int old_score = game.score;
    int old_tail_x = game.snake_x[game.tail_slot];
    int old_tail_y = game.snake_y[game.tail_slot];
    int old_head_x = game.snake_x[game.head_slot];
    int old_head_y = game.snake_y[game.head_slot];
    advance_game(&game);
    if (game.score != old_score) {
      maybe_record_high_score(game.score);
    }
    if (!game.alive) {
      commit_high_score();
    }

    if (!game.alive || game.won) {
      dirty = true;
    } else if (game.score != old_score) {
      redraw_growth_delta(&game, old_head_x, old_head_y);
    } else {
      redraw_move_delta(&game, old_tail_x, old_tail_y, old_head_x, old_head_y);
    }
  }

  return !quit_requested;
}

// Poll all queued key events so gameplay is driven by the newest pressed key.
static bool poll_input(struct SnakeGame* state, bool* paused,
                       bool* restart_requested, bool* quit_requested) {
  int key = 0;
  bool changed = false;

  while ((key = getkey()) != 0) {
    int direction;

    if ((key & KEY_RELEASE_MASK) != 0) continue;
    key &= KEY_ASCII_MASK;

    if (key == KEY_QUIT || key == KEY_QUIT_ALT) {
      *quit_requested = true;
      changed = true;
      continue;
    }

    if (key == KEY_RESTART || key == KEY_RESTART_ALT) {
      *restart_requested = true;
      changed = true;
      continue;
    }

    if (key == KEY_PAUSE || key == KEY_PAUSE_ALT) {
      *paused = !(*paused);
      changed = true;
      continue;
    }

    direction = direction_from_key(key);
    queue_direction(state, direction);
  }

  return changed;
}

// Print one two-value failure line before panicking the smoke test.
static void smoke_expect_eq(char* fmt, int got, int expected, char* panic_msg) {
  if (got != expected) {
    int args[2] = { got, expected };
    say(fmt, args);
    panic(panic_msg);
  }
}

// Run one deterministic non-VGA verification pass over the shared game engine.
static void run_smoke_test(void) {
  int start_head_x;
  int start_head_y;
  int safety_budget;

  say("***snake smoke start\n", NULL);

  reset_game(&game, INITIAL_RNG_STATE);
  start_head_x = game.snake_x[game.head_slot];
  start_head_y = game.snake_y[game.head_slot];
  set_food(&game, start_head_x + 1, start_head_y);
  advance_game(&game);

  smoke_expect_eq("***snake FAIL growth score=%d expected=%d\n",
                  game.score, 1,
                  "snake smoke: eating food did not increment score.\n");
  smoke_expect_eq("***snake FAIL growth length=%d expected=%d\n",
                  game.length, INITIAL_LENGTH + 1,
                  "snake smoke: eating food did not grow the snake.\n");
  smoke_expect_eq("***snake FAIL growth head_x=%d expected=%d\n",
                  game.snake_x[game.head_slot], start_head_x + 1,
                  "snake smoke: snake head did not advance into food.\n");
  smoke_expect_eq("***snake FAIL growth head_y=%d expected=%d\n",
                  game.snake_y[game.head_slot], start_head_y,
                  "snake smoke: horizontal growth changed the row unexpectedly.\n");
  say("***snake growth ok\n", NULL);

  reset_game(&game, INITIAL_RNG_STATE);
  start_head_x = game.snake_x[game.head_slot];
  queue_direction(&game, DIR_LEFT);
  advance_game(&game);

  smoke_expect_eq("***snake FAIL reverse direction=%d expected=%d\n",
                  game.direction, DIR_RIGHT,
                  "snake smoke: reverse turn should have been rejected.\n");
  smoke_expect_eq("***snake FAIL reverse head_x=%d expected=%d\n",
                  game.snake_x[game.head_slot], start_head_x + 1,
                  "snake smoke: reverse turn moved the snake backward.\n");
  say("***snake reverse-turn guard ok\n", NULL);

  load_self_collision_shape(&game);
  advance_game(&game);
  smoke_expect_eq("***snake FAIL self_collision alive=%d expected=%d\n",
                  game.alive, false,
                  "snake smoke: self collision did not end the game.\n");
  say("***snake self-collision ok\n", NULL);

  reset_game(&game, INITIAL_RNG_STATE);
  safety_budget = BOARD_WIDTH + BOARD_HEIGHT;
  while (game.alive && safety_budget > 0) {
    advance_game(&game);
    safety_budget--;
  }

  smoke_expect_eq("***snake FAIL wall alive=%d expected=%d\n",
                  game.alive, false,
                  "snake smoke: wall collision did not end the game.\n");
  assert(safety_budget > 0,
         "snake smoke: wall collision loop exhausted its safety budget.\n");
  say("***snake wall collision ok\n", NULL);

  say("***snake smoke complete\n", NULL);
}

// Run the interactive VGA game until the user quits back to the kernel harness.
static void run_interactive_game(void) {
  unsigned seed = INITIAL_RNG_STATE ^ current_jiffies;
  bool keep_running = true;

  mount_high_score_fs();
  load_text_tiles();
  install_game_tiles();
  clear_screen();
  // docs/mem_map.md defines TILE_SCALE as a power-of-two pixel multiplier.
  // Use scale 1 so each 8x8 game tile is displayed at 16x16 on screen.
  *TILE_SCALE = 1;
  *TILE_VSCROLL = 0;
  drain_keys();

  while (keep_running) {
    keep_running = play_round(seed, &seed);
  }

  unmount_high_score_fs();
}

int kernel_main(void) {
  if (CONFIG.use_vga) {
    run_interactive_game();
  } else {
    run_smoke_test();
  }

  return 0;
}
