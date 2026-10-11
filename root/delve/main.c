#include "delve.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"

/*
 * DELVE entry point: takes over the display from the terminal, runs the main
 * loop, and hands the display back on exit.
 *
 * Display ownership: the terminal process keeps running while we play. We
 * hide its cursor (so its blink timer stops writing the tile framebuffer) and
 * never print while the game owns the screen. On exit the shell sees that we
 * used direct display traps and asks the terminal to restore its own tiles,
 * scale, scroll, and sprites (docs/terminal.md, "Foreground display
 * recovery"), so we do not need to undo our VGA changes ourselves.
 *
 * Input: the terminal consumes raw keyboard events and forwards only key
 * presses, as bytes, to our STDIN; there are no key-release events and no
 * auto-repeat. All controls are therefore press-driven. The mouse stream is
 * read directly with getmouse().
 */

volatile short* g_tilemap;
volatile short* g_tile_fb;
volatile short* g_spritemap;
unsigned g_view_words[7680];
unsigned g_frame;

/* Frames (60 Hz) a step or a quarter turn takes. */
#define STEP_FRAMES 12
#define TURN_FRAMES 10

/* Player on the grid; the camera interpolates between grid poses. */
struct Player {
  int cx;
  int cy;
  int facing;      /* 0 east, 1 south, 2 west, 3 north */
  /* active animation */
  int anim;        /* 0 none, 1 move, 2 turn */
  int anim_t;      /* frames elapsed */
  int anim_len;
  int from_x;
  int from_y;
  int to_x;
  int to_y;
  int from_angle;
  int to_angle;
  int queued;      /* next command byte, 0 if none */
};

static struct Player player;
static struct Camera camera;
static bool running = true;

static int dir_dx[4] = {1, 0, -1, 0};
static int dir_dy[4] = {0, 1, 0, -1};

/* Smoothstep easing of t/len in 16.16 (0..1, zero slope at both ends). */
static int ease(int t, int len){
  int u = (t << 16) / len;
  /* u * u * (3 - 2u) */
  int u2 = fixmul(u, u);
  return fixmul(u2, 3 * FIX_ONE - 2 * u);
}

static int cell_center(int c){
  return (c << 16) + FIX_HALF;
}

/* Start a move to the neighbor in direction `dir` if it is open. */
static void try_move(int dir){
  int nx = player.cx + dir_dx[dir];
  int ny = player.cy + dir_dy[dir];
  if (cell_solid(nx, ny)){
    return;
  }
  player.anim = 1;
  player.anim_t = 0;
  player.anim_len = STEP_FRAMES;
  player.from_x = cell_center(player.cx);
  player.from_y = cell_center(player.cy);
  player.to_x = cell_center(nx);
  player.to_y = cell_center(ny);
  player.cx = nx;
  player.cy = ny;
}

/* Start a quarter turn: +1 clockwise (right), -1 counterclockwise (left). */
static void try_turn(int delta){
  player.anim = 2;
  player.anim_t = 0;
  player.anim_len = TURN_FRAMES;
  player.from_angle = player.facing * ANGLE_QUARTER;
  player.to_angle = player.from_angle + delta * ANGLE_QUARTER;
  player.facing = (player.facing + delta) & 3;
}

/* Begin the animation for one command byte from the terminal. */
static void start_command(int key){
  int f = player.facing;
  switch (key){
    case 'w': case 'W': case KEY_UP:    try_move(f); break;
    case 's': case 'S': case KEY_DOWN:  try_move((f + 2) & 3); break;
    case 'a': case 'A':                 try_move((f + 3) & 3); break;
    case 'd': case 'D':                 try_move((f + 1) & 3); break;
    case 'q': case 'Q': case KEY_LEFT:  try_turn(-1); break;
    case 'e': case 'E': case KEY_RIGHT: try_turn(1); break;
    case 'x': case 'X':                 running = false; break;
    default: break;
  }
}

/* Advance the player's animation by one 60 Hz tick and update the camera. */
static void update_player(void){
  if (player.anim == 0 && player.queued){
    int key = player.queued;
    player.queued = 0;
    start_command(key);
  }
  if (player.anim != 0){
    player.anim_t += 1;
    int k = ease(player.anim_t, player.anim_len);
    if (player.anim == 1){
      camera.x = player.from_x + fixmul(player.to_x - player.from_x, k);
      camera.y = player.from_y + fixmul(player.to_y - player.from_y, k);
    } else {
      camera.angle = (player.from_angle +
                      fixmul(player.to_angle - player.from_angle, k)) & ANGLE_MASK;
    }
    if (player.anim_t >= player.anim_len){
      player.anim = 0;
      camera.x = cell_center(player.cx);
      camera.y = cell_center(player.cy);
      camera.angle = player.facing * ANGLE_QUARTER;
    }
  }
}

/* Drain pending terminal bytes into the one-command input buffer. */
static void poll_keys(void){
  int avail = fd_bytes_available(STDIN);
  while (avail > 0){
    unsigned char byte = 0;
    if (read(STDIN, &byte, 1) != 1){
      break;
    }
    avail -= 1;
    if (player.anim == 0 && player.queued == 0){
      start_command(byte);
    } else {
      player.queued = byte;
    }
  }
}

/* Take over the tile layer and status-bar sprites. */
static void display_init(void){
  /* Clear the terminal and hide its cursor, then give the terminal time to
   * act on that before we start writing the tile framebuffer. */
  puts("\x1b[2J\x1b[?25l");
  sleep(30);

  g_tilemap = get_tilemap();
  g_tile_fb = get_tile_fb();
  g_spritemap = get_spritemap();

  set_tile_scale(TILE_SCALE_4X);
  set_hscroll(0);
  set_vscroll(0);

  /* Blank tile: opaque black. */
  for (int i = 0; i < TILEMAP_SHORTS_PER_TILE; ++i){
    g_tilemap[BLANK_TILE * TILEMAP_SHORTS_PER_TILE + i] = 0;
  }
  /* View tiles, column-major; everything else blank. */
  for (int ty = 0; ty < TILE_FB_ROWS; ++ty){
    for (int tx = 0; tx < TILE_FB_COLS; ++tx){
      int tile = BLANK_TILE;
      if (tx < VIEW_TILE_COLS && ty < VIEW_TILE_ROWS){
        tile = tx * VIEW_TILE_ROWS + ty;
      }
      g_tile_fb[ty * TILE_FB_COLS + tx] = tile;
    }
  }
  /* Hide every sprite, then place the five status-bar sprites. */
  for (int s = 0; s < 16; ++s){
    set_sprite_coords(s, SPRITE_HIDDEN, SPRITE_HIDDEN);
  }
  for (int s = 0; s < HUD_SPRITE_COUNT; ++s){
    int sprite = HUD_SPRITE_FIRST + s;
    for (int i = 0; i < SPRITE_SHORTS; ++i){
      g_spritemap[sprite * SPRITE_SHORTS + i] = 0x0111;
    }
    set_sprite_scale(sprite, TILE_SCALE_4X);
    set_sprite_coords(sprite, s * 32, HUD_Y);
  }
}

int main(void){
  display_init();
  render_init();
  world_init_test_map();

  player.cx = 2;
  player.cy = 2;
  player.facing = 0;
  camera.x = cell_center(player.cx);
  camera.y = cell_center(player.cy);
  camera.angle = 0;

  fill_rect(SURF_HUD, 0, 0, HUD_W, HUD_H, 0x0221);
  draw_text(SURF_HUD, 4, 4, "DELVE ENGINE TEST", 0x0FFF);
  draw_big_text(SURF_HUD, 4, 11, "Dioptase 1234", 0x04F8, 1);
  hud_upload();

  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int fps_frames = 0;
  int fps = 0;
  char fps_text[16];
  while (running){
    unsigned now = get_vga_frame_counter();
    unsigned ticks = now - last;
    last = now;
    if (ticks > 6) ticks = 6;
    poll_keys();
    for (unsigned t = 0; t < ticks; ++t){
      update_player();
      g_frame += 1;
    }
    render_view(&camera);
    fps_frames += 1;
    if (now - fps_mark >= 60){
      fps = fps_frames * 60 / (now - fps_mark);
      fps_frames = 0;
      fps_mark = now;
    }
    draw_text_shadow(SURF_VIEW, 2, 2, format_int(fps_text, fps), 0x0FF0);
    blit_view(g_tilemap, g_view);
    hud_upload();
  }
  return 0;
}
