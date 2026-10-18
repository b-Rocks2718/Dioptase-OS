#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "pak.h"

/*
 * RETAKE: a stunt double, a cursed film studio, and the takes she records.
 * By Claude Opus and GPT-6 Astra.
 *
 * Main loop: the game advances in fixed 60 Hz ticks paced by the VGA frame
 * counter, then composites and presents one frame. Bring-up build: one test
 * room, the stub controller, and a frames-per-second readout.
 */

/* Hero animation frames, in tools/sprites.py HERO_ORDER. */
#define FR_IDLE0 0
#define FR_IDLE1 1
#define FR_RUN0 2
#define FR_RUN1 3
#define FR_RUN2 4
#define FR_JUMP 5
#define FR_FALL 6
#define FR_CLAP0 7
#define FR_CLAP1 8
#define FR_WALL 9
#define FR_HURT 10

/* Where the body hitbox's top-left sits on the 32x32 sprite canvas
 * (tools/sprites.py: CENTER_X 16, FEET_Y 27). */
#define CANVAS_BODY_X 12
#define CANVAS_BODY_Y 13

static struct Player hero;
static int cam_x;
static int cam_y;
static unsigned tick_count;
static int anim_clock;

/* Pick the hero's animation frame from controller state. */
static int hero_frame(void) {
  if (hero.hurt_timer > 0) return FR_HURT;
  if (hero.slash_timer > 6) return FR_CLAP0;
  if (hero.slash_timer > 0) return FR_CLAP1;
  if (hero.wall_dir != 0) return FR_WALL;
  if (!hero.on_ground) return hero.vy < 0 ? FR_JUMP : FR_FALL;
  if (hero.vx > 64 || hero.vx < -64) {
    int phase = (anim_clock >> 3) & 3;
    if (phase == 0) return FR_RUN0;
    if (phase == 2) return FR_RUN2;
    return FR_RUN1;
  }
  return ((anim_clock >> 5) & 1) ? FR_IDLE1 : FR_IDLE0;
}

/* Ease the camera toward the hero and keep it inside the room. */
static void update_camera(int snap) {
  int hx = (hero.x >> 8) + (BODY_W >> 1);
  int hy = (hero.y >> 8) + (BODY_H >> 1);
  int tx = hx - (SCREEN_W >> 1);
  int ty = hy - (VIEW_H >> 1) - 8;
  if (snap) {
    cam_x = tx;
    cam_y = ty;
  } else {
    cam_x += (tx - cam_x) >> 3;
    cam_y += (ty - cam_y) >> 3;
  }
  int max_x = g_room_pw - SCREEN_W;
  int max_y = g_room_ph - VIEW_H;
  if (cam_x > max_x) cam_x = max_x;
  if (cam_y > max_y) cam_y = max_y;
  if (cam_x < 0) cam_x = 0;
  if (cam_y < 0) cam_y = 0;
}

/* Show a number with the terminal font's digit tiles in the top letterbox. */
static void show_number(int n, int col) {
  for (int i = 3; i >= 0; i--) {
    int digit = n % 10;
    n = n / 10;
    g_tile_fb[col + i] = (short)((0xFF << 8) | ('0' + digit));
  }
}

static void respawn(void) {
  struct PakRoom* pr = pak_room(g_room_index);
  player_init(&hero, pr->spawn_x << 3, (pr->spawn_y << 3) + TILE - BODY_H, 0);
}

int main(void) {
  if (pak_open() != 0) return 1;
  room_system_init();
  if (display_init() != 0) return 1;
  input_init();
  hero_sprite_init();
  room_load(ROOM_TEST_STREET);
  render_region(pak_room(g_room_index)->region - 1);
  respawn();
  update_camera(1);

  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int frames = 0;
  int running = 1;
  while (running) {
    unsigned now = get_vga_frame_counter();
    while (now == last) now = get_vga_frame_counter();
    int ticks = (int)(now - last);
    if (ticks > 4) ticks = 4;
    last = now;
    input_poll();
    if (input_key_hit(KEY_ESC)) running = 0;
    int held = input_held();
    int pressed = input_take_pressed();
    for (int t = 0; t < ticks; t++) {
      player_step(&hero, &g_room, held, pressed);
      pressed = 0;
      if (hero.events & EV_HAZARD) respawn();
      tick_count++;
      anim_clock++;
    }
    update_camera(0);
    render_frame(cam_x, cam_y);
    hero_sprite_place(hero_frame(), hero.facing,
                      (hero.x >> 8) - cam_x - CANVAS_BODY_X,
                      (hero.y >> 8) - cam_y - CANVAS_BODY_Y);
    frames++;
    if (now - fps_mark >= 60) {
      show_number(frames * 60 / (int)(now - fps_mark), TILE_ROW_WIDTH - 5);
      frames = 0;
      fps_mark = now;
    }
  }
  input_shutdown();
  display_shutdown();
  return 0;
}
