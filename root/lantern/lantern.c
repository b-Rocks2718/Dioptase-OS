/*
 * Lantern: a first-person walk through a sealed vault.
 *
 * The picture is a 160x120 raycast in the pixel framebuffer at scale 1, so
 * each pixel is 4 screen pixels (docs/mem_map.md). The tile layer is cleared
 * to the transparent tile except for the HUD. WASD comes from raw key events
 * on stdin, including releases, so movement is held rather than tapped.
 * Mouse look uses getmouse(); the right button walks forward.
 *
 * From the shell, with VGA on: lantern
 * Headless timing: lantern bench
 */

#include "lantern.h"
#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/mouse.h"
#include "../crt/ps2.h"
#include "../crt/string.h"
#include "../crt/print.h"

int lantern_bench;

static int direct_keys;
static int held_forward;
static int held_back;
static int held_left;
static int held_right;
static int held_turn_left;
static int held_turn_right;
static int mouse_dx;
static int mouse_buttons;
static int mouse_right;
static int edge_fire;
static int edge_use;
static int edge_map;
static int edge_pause;
static int edge_quit;
static int edge_restart;
static int edge_start;
static int show_map;

/* Forget one-shot keys. Held movement stays down until a release arrives. */
static void clear_edges(void) {
  edge_fire = 0;
  edge_use = 0;
  edge_map = 0;
  edge_pause = 0;
  edge_quit = 0;
  edge_restart = 0;
  edge_start = 0;
}

/* Apply one PS/2 key. down is 0 on release. */
static void set_held(int code, int down) {
  if (code == 'w' || code == KEY_UP) held_forward = down;
  else if (code == 's' || code == KEY_DOWN) held_back = down;
  else if (code == 'a') held_left = down;
  else if (code == 'd') held_right = down;
  else if (code == KEY_LEFT) held_turn_left = down;
  else if (code == KEY_RIGHT) held_turn_right = down;
  if (!down) return;
  if (code == 'f' || code == ' ') edge_fire = 1;
  if (code == 'e') edge_use = 1;
  if (code == 'm') edge_map = 1;
  if (code == 'p') edge_pause = 1;
  if (code == 'q' || code == 0x1B) edge_quit = 1;
  if (code == 'r') edge_restart = 1;
  if (code == ' ' || code == KEY_ENTER) edge_start = 1;
}

/* Next raw keyboard word, or 0 when none is waiting. */
static int read_key_event(void) {
  unsigned char bytes[2];
  int available;
  if (direct_keys) return getkey();
  available = fd_bytes_available(STDIN);
  if (available < 2) return 0;
  if (read(STDIN, bytes, 2) != 2) return 0;
  return bytes[0] | (bytes[1] << 8);
}

/* Drain every queued key and mouse event into the held and edge state. */
static void poll_input(void) {
  int ev;
  while ((ev = read_key_event()) != 0) {
    set_held(ev & 0xFF, (ev & KEY_RELEASE_BIT) == 0);
  }
  while ((ev = getmouse()) != 0) {
    int buttons = MOUSE_BUTTONS(ev);
    int dx = MOUSE_DX(ev);
    if (dx > 40) dx = 40;
    if (dx < -40) dx = -40;
    mouse_dx += dx;
    if ((buttons & MOUSE_BUTTON_LEFT) && !(mouse_buttons & MOUSE_BUTTON_LEFT)) {
      edge_fire = 1;
      edge_start = 1;
    }
    if (buttons & MOUSE_BUTTON_RIGHT) edge_start = 1;
    mouse_right = (buttons & MOUSE_BUTTON_RIGHT) != 0;
    mouse_buttons = buttons;
  }
}

/* Drop cooked bytes left in the pipe so the first raw pair stays aligned. */
static void drain_stdin(void) {
  unsigned char byte;
  while (fd_bytes_available(STDIN) > 0) {
    if (read(STDIN, &byte, 1) != 1) return;
  }
}

/* Display frames since last. Waits for vblank when the previous frame was fast. */
static int take_dt(unsigned* last) {
  unsigned now = get_vga_frame_counter();
  unsigned dt = now - *last;
  int spins = 0;
  if (dt == 0) {
    while (get_vga_frame_counter() == now && spins < 2000) {
      audio_pump();
      poll_input();
      spins++;
    }
    now = get_vga_frame_counter();
    dt = now - *last;
    if (dt == 0) dt = 1;
  }
  if (dt > 6) dt = 6;
  *last = now;
  return (int)dt;
}

/* Map the display, build the tables, and optionally open audio and raw keys. */
static int startup(int bench) {
  volatile short* pixel_fb;
  volatile short* tile_fb;
  volatile short* sprite_fb;

  lantern_bench = bench;
  if (!bench) {
    puts("\x1b[?25l\x1b[2J");
    sleep(80);
  }
  tile_fb = get_tile_fb();
  pixel_fb = get_pixel_fb();
  sprite_fb = get_spritemap();
  if (pixel_fb == (volatile short*)-1 || tile_fb == (volatile short*)-1 ||
      sprite_fb == (volatile short*)-1) {
    puts("lantern: display mapping failed\n");
    return -1;
  }
  load_text_tiles();
  set_pixel_scale(PIXEL_SCALE_4X);
  if (render_init(pixel_fb, tile_fb, sprite_fb) != 0) {
    puts("lantern: the view buffer could not be allocated\n");
    return -1;
  }
  if (load_level() != 0) {
    puts("lantern: the vault map is the wrong size\n");
    return -1;
  }
  if (!bench) {
    audio_start();
    if (fd_bytes_available(STDIN) < 0) {
      direct_keys = 1;
    } else {
      set_keyboard_raw(1);
      drain_stdin();
    }
  }
  show_map = 1;
  return 0;
}

/* Restore the console scale and release input and audio. */
static void shutdown(void) {
  render_hide_sprites();
  set_pixel_scale(0);
  if (!lantern_bench) {
    set_keyboard_raw(0);
    audio_stop();
  }
}

/* Rotate and step a few frames, then print a checksum and an ASCII view. */
static int run_bench(void) {
  unsigned start;
  int i;
  int args[2];
  if (startup(1) != 0) return 1;
  start = get_current_jiffies();
  for (i = 0; i < 8; i++) {
    player_angle = (player_angle + 5) & (ANGLE_FULL - 1);
    lantern_time += 1;
    render_frame(STATE_PLAY, 0);
  }
  for (i = 0; i < 4; i++) {
    player_x += 40;
    lantern_time += 1;
    render_frame(STATE_PLAY, 0);
  }
  args[0] = (int)(get_current_jiffies() - start);
  args[1] = (int)view_checksum();
  printf("***lantern bench jiffies=%u checksum=%u\n", args);
  view_preview();
  shutdown();
  return 0;
}

/* Play until Q, Escape, or Ctrl-C. */
static int run_game(void) {
  unsigned last_frame;
  int state = STATE_TITLE;
  if (startup(0) != 0) return 1;
  last_frame = get_vga_frame_counter();
  while (!edge_quit) {
    int dt;
    int turn;
    int forward;
    clear_edges();
    dt = take_dt(&last_frame);
    poll_input();
    lantern_time += (unsigned)dt;
    if (hurt_timer > 0) {
      hurt_timer -= dt;
      if (hurt_timer < 0) hurt_timer = 0;
    }
    turn = mouse_dx * 2;
    mouse_dx = 0;
    if (held_turn_left) turn -= 12 * dt;
    if (held_turn_right) turn += 12 * dt;
    if (turn > 120) turn = 120;
    if (turn < -120) turn = -120;
    forward = held_forward || mouse_right;

    if (state == STATE_TITLE) {
      if (edge_start) {
        state = STATE_PLAY;
        player_angle = 0;
        world_dirty = 1;
        set_message("Three cores seal the gate.", 160);
      } else {
        player_angle = (player_angle + dt * 2) & (ANGLE_FULL - 1);
      }
    } else if (state == STATE_PLAY) {
      int next = update_world(dt, forward, held_back, held_left, held_right, turn, edge_use, edge_fire);
      if (edge_map) show_map = !show_map;
      if (edge_pause) state = STATE_PAUSE;
      else if (edge_restart) {
        load_level();
        state = STATE_PLAY;
        set_message("The vault resets.", 100);
      } else if (next != STATE_PLAY) {
        state = next;
      }
    } else if (state == STATE_PAUSE) {
      if (edge_pause || edge_start) state = STATE_PLAY;
    } else if (edge_restart || edge_start) {
      load_level();
      state = STATE_PLAY;
      set_message("The vault resets.", 100);
    }
    render_frame(state, show_map);
  }
  shutdown();
  return 0;
}

/* Enter the vault, or render a headless bench when argv is "bench". */
int main(int argc, char** argv) {
#ifdef LANTERN_FORCE_BENCH
  (void)argc;
  (void)argv;
  return run_bench();
#else
  if (argc > 1 && streq(argv[1], "bench")) {
    return run_bench();
  }
  return run_game();
#endif
}
