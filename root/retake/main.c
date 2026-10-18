#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "takes.h"
#include "pak.h"

/*
 * RETAKE: a stunt double, a cursed film studio, and the takes she records.
 * By Claude Opus and GPT-6 Astra.
 *
 * Main loop: the game advances in fixed 60 Hz ticks paced by the VGA frame
 * counter, then composites and presents one frame. Each tick follows the
 * movement core's documented order (takes.h): advance projections, offer
 * them as platforms, step the live player, then capture her resolved pose
 * for a recording in progress. Jump cuts happen after the capture.
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
#define FR_GLIDE 11

/* Hardware sprite slots. */
#define SLOT_HERO 0
#define SLOT_TAKE0 1

/* Where the 8x12 body's top-left sits on the 32x32 sprite canvas
 * (tools/sprites.py: CENTER_X 16, FEET_Y 27). */
#define CANVAS_BODY_X 12
#define CANVAS_BODY_Y 15

static struct Player hero;
static struct Takes takes;
static struct Platform platforms[3];
static int cam_x;
static int cam_y;
static unsigned tick_count;
static int rec_ticks;

/* Animation frame for a pose. Running strides advance with distance, so the
 * legs keep pace with the speed; `x` is the body's Q8 x position. */
static int frame_for(int pose, int clapping_active, int x, int clock) {
  switch (pose) {
    case POSE_RUN: {
      int phase = (x >> 11) & 3;   /* a new leg position every 8 pixels */
      if (phase == 0) return FR_RUN0;
      if (phase == 2) return FR_RUN2;
      return FR_RUN1;
    }
    case POSE_RISE: return FR_JUMP;
    case POSE_FALL: return FR_FALL;
    case POSE_WALL: return FR_WALL;
    case POSE_GLIDE: return FR_GLIDE;
    case POSE_CLAP: return clapping_active ? FR_CLAP1 : FR_CLAP0;
    case POSE_HURT: return FR_HURT;
    default: return ((clock >> 5) & 1) ? FR_IDLE1 : FR_IDLE0;
  }
}

/* Ease the camera toward the hero and keep it inside the room. */
static void update_camera(int snap) {
  int hx = (hero.x >> 8) + (PLAYER_W >> 1);
  int hy = (hero.y >> 8) + (PLAYER_H >> 1);
  int tx = hx - (SCREEN_W >> 1) + (hero.facing << 4);
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

/* Write ASCII text into the tile layer using the terminal font tiles that are
 * still loaded (tile index = character code), colored with an RGB332 byte. */
static void put_text(int col, int row, char* s, int color) {
  int i = 0;
  while (s[i]) {
    g_tile_fb[row * TILE_ROW_WIDTH + col + i] = (short)((color << 8) | (s[i] & 0xFF));
    i++;
  }
}

static void show_number(int n, int col, int row, int color) {
  for (int i = 3; i >= 0; i--) {
    int digit = n % 10;
    n = n / 10;
    g_tile_fb[row * TILE_ROW_WIDTH + col + i] = (short)((color << 8) | ('0' + digit));
  }
}

static void respawn(void) {
  struct PakRoom* pr = pak_room(g_room_index);
  player_init(&hero, pr->spawn_x << 3, (pr->spawn_y << 3) + TILE - PLAYER_H, AB_JUMPCUT | AB_WIRE | AB_GLIDE);
  takes_init(&takes, 3, TAKE_BASE_FRAMES);
}

/* Jump-cut to the newest take that accepts the destination. */
static void try_jumpcut(void) {
  int limit = takes.slot_limit;
  int slot = takes.replace_next;
  for (int i = 0; i < limit; i++) {
    slot = slot - 1;
    if (slot < 0) slot = limit - 1;
    if (takes_jumpcut(&takes, &hero, &g_room, slot)) return;
  }
}

/* One fixed simulation tick in the movement core's documented order. */
static void tick(int held, int pressed) {
  takes_advance(&takes);
  g_room.platform_count = takes_platforms(&takes, platforms);
  g_room.platforms = platforms;
  player_step(&hero, &g_room, held, pressed);
  int rec = takes_capture(&takes, &hero, (pressed & IN_RECORD) != 0);
  if (rec == 1) rec_ticks = 0;
  if (takes.recording) rec_ticks++;
  if ((pressed & IN_JUMPCUT) && (hero.abilities & AB_JUMPCUT)) try_jumpcut();
  if (hero.events & EV_HAZARD) respawn();
  tick_count++;
}

static void draw_sprites(void) {
  struct Hitbox box;
  int active = player_attack_box(&hero, &box);
  int f = frame_for(hero.pose, active, hero.x, (int)tick_count);
  sprite_show(SLOT_HERO, SHEET_HERO, f, hero.facing,
              (hero.x >> 8) - cam_x - CANVAS_BODY_X, (hero.y >> 8) - cam_y - CANVAS_BODY_Y);
  int sheet = ((tick_count >> 1) & 1) ? SHEET_GHOST_B : SHEET_GHOST_A;
  for (int i = 0; i < TAKE_MAX_SLOTS; i++) {
    struct Take* t = &takes.slots[i];
    if (!t->active || t->length == 0) {
      sprite_hide(SLOT_TAKE0 + i);
      continue;
    }
    struct TakeFrame* fr = &t->frames[t->cursor];
    int tf = frame_for(fr->pose, fr->attack_dir != 0, fr->x, (int)tick_count);
    sprite_show(SLOT_TAKE0 + i, sheet, tf, fr->facing,
                (fr->x >> 8) - cam_x - CANVAS_BODY_X, (fr->y >> 8) - cam_y - CANVAS_BODY_Y);
  }
}

/* Top letterbox: a REC light while recording. */
static void draw_status(void) {
  if (takes.recording) {
    int blink = (tick_count >> 4) & 1;
    put_text(2, 0, blink ? "* REC" : "  REC", 0xE0);
    show_number(rec_ticks, 8, 0, 0xE0);
  } else {
    put_text(2, 0, "           ", 0);
  }
}

int main(void) {
  if (pak_open() != 0) return 1;
  room_system_init();
  if (display_init() != 0) return 1;
  input_init();
  sprites_init();
  sheet_load(SHEET_HERO);
  sheet_load(SHEET_GHOST_A);
  sheet_load(SHEET_GHOST_B);
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
      tick(held, pressed);
      pressed = 0;
    }
    update_camera(0);
    render_frame(cam_x, cam_y);
    draw_sprites();
    draw_status();
    frames++;
    if (now - fps_mark >= 60) {
      show_number(frames * 60 / (int)(now - fps_mark), TILE_ROW_WIDTH - 5, 0, 0xFF);
      frames = 0;
      fps_mark = now;
    }
  }
  input_shutdown();
  display_shutdown();
  return 0;
}
