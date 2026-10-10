/*
 * Starfall is a five-wave arcade survival game for the Dioptase tile display.
 * It runs in user mode and reaches the keyboard, VGA and PCM device only via
 * the documented Dioptase-OS syscalls. The terminal forwards key presses to
 * STDIN while this process is foreground; each press refreshes a short movement
 * latch so ordinary keyboard repeat gives smooth steering without assuming
 * release events survive that pipe.
 */

#include "starfall.h"
#include "graphics.h"
#include "sound.h"

#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/fcntl.h"
#include "../crt/print.h"
#include "../crt/string.h"

#define RECORD_PATH "/starfall/record.txt"
#define TERMINAL_HANDOFF_FRAMES 8
#define VGA_PROBE_JIFFIES 3000
#define INPUT_EVENTS_PER_FRAME 32

/* Large bounded pools live in static user memory, outside the 16 KiB stack. */
static struct Game game;

/* The guest check below reuses the normal persistence routines. */
static unsigned load_best(void);
static bool save_best(unsigned best);

/* Exercise the compiled guest rules when no graphical host is available. */
static int run_guest_check(void) {
  game_init(&game, 0xA5A5A5A5u, 0);
  struct GameInput input;
  input.move_x = 0;
  input.move_y = 0;
  input.move_pressed = 0;
  input.pulse = 0;
  input.start = 1;
  input.choose = 0;
  game_tick(&game, input);
  if (game.phase != PHASE_PLAY || game.hp != 6 ||
      game.cells[game.player_y * FIELD_W + game.player_x] != CELL_SPACE) {
    puts("***FAIL starfall guest launch or center spawn\n");
    return 1;
  }
  game.enemies[0].active = 1;
  game.enemies[0].kind = ENEMY_HUNTER;
  game.enemies[0].x = game.player_x + 2;
  game.enemies[0].y = game.player_y;
  game.enemies[0].hp = 2;
  game.enemies[0].move_timer = 100;
  input.start = 0;
  input.pulse = 1;
  game_tick(&game, input);
  if (game.enemies[0].active || game.pulse_charge != 0 || game.kills != 1) {
    puts("***FAIL starfall guest nova collision or charge\n");
    return 1;
  }
  game.spawn_budget = 0;
  game.spawned = 0;
  input.pulse = 0;
  game_tick(&game, input);
  if (game.phase != PHASE_UPGRADE) {
    puts("***FAIL starfall guest wave progression\n");
    return 1;
  }
  if (!graphics_init()) {
    puts("***FAIL starfall guest tile device mapping\n");
    return 1;
  }
  graphics_draw(&game, false);
  input.choose = 1;
  game_tick(&game, input);
  if (game.phase != PHASE_PLAY || game.wave != 2) {
    puts("***FAIL starfall guest salvage choice or next wave\n");
    return 1;
  }
  graphics_draw(&game, false);
  sound_init();
  sound_tick(&game);
  sound_shutdown();
  if (!save_best(1234) || load_best() != 1234) {
    puts("***FAIL starfall guest save/read /starfall/record.txt\n");
    return 1;
  }
  puts("***PASS starfall guest engine\n");
  return 0;
}

/* Parse a small decimal record, accepting only digits and a final newline. */
static unsigned load_best(void) {
  int fd = open_existing(RECORD_PATH);
  if (fd < 0) return 0;
  char text[16];
  int length = read(fd, text, sizeof(text));
  close(fd);
  if (length <= 0) return 0;
  unsigned value = 0;
  for (int i = 0; i < length; ++i) {
    if (text[i] == '\n') break;
    if (text[i] < '0' || text[i] > '9') return 0;
    unsigned digit = (unsigned)(text[i] - '0');
    if (value > (0xFFFFFFFFu - digit) / 10) return 0;
    value = value * 10 + digit;
  }
  return value;
}

/* Persist a new record through the documented creating-open and truncate calls. */
static bool save_best(unsigned best) {
  int fd = open(RECORD_PATH);
  if (fd < 0) return false;
  char text[12];
  int count = 0;
  do {
    text[count++] = (char)('0' + best % 10);
    best /= 10;
  } while (best != 0);
  for (int i = 0; i < count / 2; ++i) {
    char swap = text[i];
    text[i] = text[count - i - 1];
    text[count - i - 1] = swap;
  }
  text[count++] = '\n';
  bool okay = truncate(fd, 0) == 0 &&
              write(fd, text, (unsigned)count) == count;
  if (close(fd) < 0) okay = false;
  return okay;
}

/* Read one queued terminal byte, or a raw make event for direct launches. */
static int next_key(void) {
  int available = fd_bytes_available(STDIN);
  if (available > 0) {
    unsigned char byte;
    if (read(STDIN, &byte, 1) == 1) return byte;
    return -1;
  }
  if (available < 0) {
    int event = getkey();
    if (event == 0) return -1;
    if (event & 0x100) return -1;
    return event & 255;
  }
  return -1;
}

/* Discard bytes typed at the preceding shell prompt before the title screen. */
static void drain_keys(void) {
  for (int i = 0; i < 256; ++i) {
    if (next_key() < 0) break;
  }
}

/* Translate press events to one frame of game actions. */
static struct GameInput collect_input(bool* paused, bool* quit) {
  struct GameInput input;
  input.move_x = 0;
  input.move_y = 0;
  input.move_pressed = 0;
  input.pulse = 0;
  input.start = 0;
  input.choose = 0;
  for (int n = 0; n < INPUT_EVENTS_PER_FRAME; ++n) {
    int key = next_key();
    if (key < 0) break;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key == 'q') {
      *quit = true;
    } else if (key == 'p' && game.phase == PHASE_PLAY) {
      *paused = !*paused;
    } else if (key == 'w' || key == 0x88) {
      input.move_x = 0;
      input.move_y = -1;
      input.move_pressed = 1;
    } else if (key == 's' || key == 0x87) {
      input.move_x = 0;
      input.move_y = 1;
      input.move_pressed = 1;
    } else if (key == 'a' || key == 0x86) {
      input.move_x = -1;
      input.move_y = 0;
      input.move_pressed = 1;
    } else if (key == 'd' || key == 0x85) {
      input.move_x = 1;
      input.move_y = 0;
      input.move_pressed = 1;
    } else if (key == ' ') {
      input.pulse = 1;
    } else if (key == '\n' || key == '\r') {
      input.start = 1;
    } else if (key >= '1' && key <= '3') {
      input.choose = key - '0';
    }
  }
  return input;
}

/* Wait for a real frame edge, yielding the CPU between device reads. */
static unsigned wait_frame(unsigned previous) {
  unsigned current;
  do {
    sleep(1);
    current = get_vga_frame_counter();
  } while (current == previous);
  return current;
}

/* Hide the terminal cursor, then let its process consume the clear sequence. */
static bool hand_off_display(void) {
  puts("\x1b[?25l\x1b[2J");
  unsigned frame = get_vga_frame_counter();
  unsigned start_jiffies = get_current_jiffies();
  while (get_vga_frame_counter() - frame < TERMINAL_HANDOFF_FRAMES) {
    if (get_current_jiffies() - start_jiffies > VGA_PROBE_JIFFIES) {
      puts("starfall: VGA frame clock did not advance; enable VGA emulation\n");
      return false;
    }
    sleep(1);
  }
  return true;
}

/* Run the foreground game until Q, then release audio and save the record. */
int main(int argc, char** argv) {
  if (argc == 2 && streq(argv[1], "--selftest")) {
    return run_guest_check();
  }
  unsigned best_on_disk = load_best();
  game_init(&game, get_current_jiffies() ^ 0x571AFA11u, best_on_disk);
  if (!hand_off_display()) return 1;
  if (!graphics_init()) {
    return 1;
  }
  sound_init();
  drain_keys();
  bool paused = false;
  bool quit = false;
  unsigned previous_frame = get_vga_frame_counter();
  graphics_draw(&game, paused);

  while (!quit) {
    previous_frame = wait_frame(previous_frame);
    struct GameInput input = collect_input(&paused, &quit);
    if (quit) break;
    if (paused) {
      game.frame++;
      game.sound_events = 0;
    } else {
      int prior_phase = game.phase;
      game_tick(&game, input);
      if (prior_phase != game.phase &&
          (game.phase == PHASE_DEAD || game.phase == PHASE_VICTORY) &&
          game.best > best_on_disk && save_best(game.best)) {
        best_on_disk = game.best;
      }
    }
    graphics_draw(&game, paused);
    sound_tick(&game);
  }

  sound_shutdown();
  if (game.best > best_on_disk && !save_best(game.best)) {
    puts("starfall: saving /starfall/record.txt failed; check the SD1 root filesystem\n");
  }
  return 0;
}
