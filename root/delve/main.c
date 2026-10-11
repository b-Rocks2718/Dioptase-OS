#include "delve.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"
#include "../crt/mouse.h"

/*
 * DELVE entry point and game flow: display ownership, the main loop, input,
 * the player, and the title/pause/death/victory screens.
 *
 * Display ownership: the terminal process keeps running while we play. We
 * hide its cursor (so its blink timer stops writing the tile framebuffer) and
 * never print while the game owns the screen. On exit the shell sees that we
 * used direct display traps and asks the terminal to restore its own tiles,
 * scale, scroll, and sprites (docs/terminal.md, "Foreground display
 * recovery"), so we do not undo our VGA changes ourselves.
 *
 * Input: the terminal consumes raw keyboard events and forwards only key
 * presses, as bytes, to our STDIN; there are no key-release events and no
 * auto-repeat. Every keyboard control is therefore a press (grid steps and
 * quarter turns). The mouse stream is ours alone (the terminal never reads
 * it), so aiming and casting use real button state from getmouse().
 *
 * Timing: game logic runs in 60 Hz ticks paced by the VGA frame counter,
 * which follows wall-clock time on both the emulator and hardware (PIT
 * jiffies count emulated cycles instead). Rendering takes whatever time is
 * left; on a slow frame several ticks run before the next render.
 */

volatile short* g_tilemap;
volatile short* g_tile_fb;
volatile short* g_spritemap;
unsigned g_view_words[7680];
unsigned g_frame;

/* Ticks a grid step or a quarter turn takes. */
#define STEP_TICKS 13
#define TURN_TICKS 11

/* Spell costs and cooldowns (ticks); speeds in cells per tick (16.16). */
#define SPARK_COST 5
#define SPARK_COOLDOWN 14
#define SPARK_SPEED 0x4800
#define FIRE_COST 22
#define FIRE_COOLDOWN 42
#define FIRE_SPEED 0x3200
#define MANA_REGEN_TICKS 5

/* Hardware sprites besides the status bar (11..15). Higher sprites draw on
 * top, so the status bar covers the bottom of the staff. */
#define STAFF_SPRITE_TOP 7      /* staff: sprites 7 and 8, stacked, scale 2 */
#define STAFF_SPRITE_BOTTOM 8
#define CURSOR_SPRITE 10        /* crosshair, scale 1 */
#define STAFF_X 116             /* staff position in scale-2 sprite units */
#define STAFF_Y 50

/* Mouse cursor bounds: the 3D view in screen pixels. */
#define CURSOR_MAX_X 639
#define CURSOR_MAX_Y 383

#define STATE_TITLE 0
#define STATE_PLAY 1
#define STATE_PAUSE 2
#define STATE_DEAD 3
#define STATE_WON 4
#define STATE_HELP 5
#define STATE_DESCEND 6
#define STATE_NAME 7

/* Player on the grid; the camera interpolates between grid poses. */
struct Player {
  int cx;
  int cy;
  int facing;      /* 0 east, 1 south, 2 west, 3 north */
  int anim;        /* 0 none, 1 move, 2 turn */
  int anim_t;
  int anim_len;
  int from_x;
  int from_y;
  int to_x;
  int to_y;
  int from_angle;
  int to_angle;
  int queued;      /* next command byte, 0 if none */
  int cooldown;    /* ticks until the next spell */
  int cast_t;      /* staff thrust animation */
  int regen;
  int walk_phase;
};

static struct Player player;
static struct Camera camera;
static bool running = true;
static int state = STATE_TITLE;
static int state_t;            /* ticks spent in the current state */
static bool demo;              /* attract mode: the autopilot plays behind the title */
static bool autopilot;         /* the autopilot drives the player (demo or '\\' key) */
static int autopilot_wait;     /* ticks before the autopilot's next decision */
static bool god_mode;          /* cheat: no damage */
static unsigned game_seed;
static bool show_map;
static bool show_fps;
static int fade;               /* light levels removed from the whole view */

static int cursor_x = 320;
static int cursor_y = 192;
static int mouse_buttons;

static char message[40];
static int message_t;

static int dir_dx[4] = {1, 0, -1, 0};
static int dir_dy[4] = {0, 1, 0, -1};

/* High score table (persisted in /delve.hi as "NAME SCORE DEPTH" lines). */
#define SCORE_SLOTS 5
static char score_names[SCORE_SLOTS][4];
static int score_values[SCORE_SLOTS];
static int score_depths[SCORE_SLOTS];
static char entry_name[4];
static int entry_len;
static int entry_slot = -1;
static int final_score;        /* the finished run, kept while the demo restarts */
static int final_depth;

static void hud_redraw(void);

/* ---- Small helpers ---- */

static int cell_center(int c){
  return (c << 16) + FIX_HALF;
}

/* Smoothstep easing of t/len in 16.16 (zero slope at both ends). */
static int ease(int t, int len){
  int u = (t << 16) / len;
  int u2 = fixmul(u, u);
  return fixmul(u2, 3 * FIX_ONE - 2 * u);
}

/* Bounded string copy that always NUL-terminates `dst` (capacity `cap`). */
static void copy_text(char* dst, char* src, int cap){
  int i = 0;
  while (src[i] && i < cap - 1){
    dst[i] = src[i];
    i += 1;
  }
  dst[i] = 0;
}

/* Show `text` centered at the top of the view for about 2.5 seconds. */
void show_message(char* text){
  copy_text(message, text, 40);
  message_t = 150;
}

/* Apply damage to the player (flash, shake, sound) and end the run at 0 HP.
 * In the attract demo only the health changes. */
void player_hurt(int amount){
  if (demo){
    g_stats.hp -= amount;
    return;
  }
  if (state != STATE_PLAY) return;
  if (god_mode) amount = 0;
  g_stats.hp -= amount;
  g_stats.hurt = 9;
  g_stats.shake = 6;
  sfx(SFX_PLAYER_HURT);
  if (g_stats.hp <= 0){
    g_stats.hp = 0;
    state = STATE_DEAD;
    state_t = 0;
    autopilot = false;
    sfx(SFX_DEATH);
    audio_music(MUSIC_NONE);
  }
  hud_redraw();
}

/* ---- High scores ---- */

static void scores_default(void){
  char* names[SCORE_SLOTS] = {"DIO", "PTA", "SE!", "CPU", "OS."};
  int values[SCORE_SLOTS] = {2500, 1500, 900, 400, 150};
  for (int i = 0; i < SCORE_SLOTS; ++i){
    copy_text(score_names[i], names[i], 4);
    score_values[i] = values[i];
    score_depths[i] = 5 - i;
  }
}

/* Parse a decimal number at s[*pos] (after spaces) and advance *pos. */
static int parse_int(char* s, int* pos){
  int v = 0;
  while (s[*pos] == ' ') *pos += 1;
  while (s[*pos] >= '0' && s[*pos] <= '9'){
    v = v * 10 + (s[*pos] - '0');
    *pos += 1;
  }
  return v;
}

/* Load the table, keeping the defaults if the file is absent. */
static void scores_load(void){
  scores_default();
  int fd = open_existing("/delve.hi");
  if (fd < 0) return;
  static char buf[256];
  int n = read(fd, buf, 255);
  close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  int pos = 0;
  for (int i = 0; i < SCORE_SLOTS && pos < n; ++i){
    for (int k = 0; k < 3; ++k){
      char c = buf[pos];
      score_names[i][k] = (c >= 32 && c < 127) ? c : '?';
      if (c) pos += 1;
    }
    score_names[i][3] = 0;
    score_values[i] = parse_int(buf, &pos);
    score_depths[i] = parse_int(buf, &pos);
    while (pos < n && buf[pos] != '\n') pos += 1;
    pos += 1;
  }
}

/* Rewrite /delve.hi from the in-memory table (one line per entry). */
static void scores_save(void){
  int fd = open("/delve.hi");
  if (fd < 0) return;
  truncate(fd, 0);
  seek(fd, 0, SEEK_SET);
  static char line[64];
  for (int i = 0; i < SCORE_SLOTS; ++i){
    char num[12];
    int len = 0;
    for (int k = 0; k < 3; ++k) line[len++] = score_names[i][k];
    line[len++] = ' ';
    format_int(num, score_values[i]);
    for (int k = 0; num[k]; ++k) line[len++] = num[k];
    line[len++] = ' ';
    format_int(num, score_depths[i]);
    for (int k = 0; num[k]; ++k) line[len++] = num[k];
    line[len++] = '\n';
    write(fd, line, len);
  }
  close(fd);
}

/* Slot the current score would take, or -1. */
static int score_rank(int score){
  for (int i = 0; i < SCORE_SLOTS; ++i){
    if (score > score_values[i]) return i;
  }
  return -1;
}

/* Insert the finished run at `slot` with the entered initials and save. */
static void score_insert(int slot){
  for (int i = SCORE_SLOTS - 1; i > slot; --i){
    copy_text(score_names[i], score_names[i - 1], 4);
    score_values[i] = score_values[i - 1];
    score_depths[i] = score_depths[i - 1];
  }
  copy_text(score_names[slot], entry_name, 4);
  score_values[slot] = final_score;
  score_depths[slot] = final_depth;
  scores_save();
}

/* ---- Levels ---- */

static void enter_level(int depth){
  int sx;
  int sy;
  int sdir;
  g_stats.depth = depth;
  g_stats.keys = 0;
  g_stats.exit_reached = 0;
  world_generate(depth, game_seed, &sx, &sy, &sdir);
  player.cx = sx;
  player.cy = sy;
  player.facing = sdir;
  player.anim = 0;
  player.queued = 0;
  camera.x = cell_center(sx);
  camera.y = cell_center(sy);
  camera.angle = sdir * ANGLE_QUARTER;
  world_tick(player.cx, player.cy, camera.x, camera.y, true);
  hud_redraw();
}

/* Fresh hero: full health and mana, no crystals, score, or kills. */
static void reset_stats(void){
  g_stats.hp = MAX_HP;
  g_stats.max_hp = MAX_HP;
  g_stats.mp = MAX_MP;
  g_stats.crystals = 0;
  g_stats.score = 0;
  g_stats.kills = 0;
  g_stats.hurt = 0;
  g_stats.shake = 0;
  player.cooldown = 0;
  player.cast_t = 0;
}

/* Start a real game at depth 1 with a new random dungeon seed. */
static void new_game(void){
  game_seed = rng_next() ^ (get_vga_frame_counter() * 2654435761u);
  demo = false;
  god_mode = false;
  autopilot = false;
  reset_stats();
  state = STATE_PLAY;
  state_t = 0;
  enter_level(1);
  fade = LIGHT_LEVELS;
  show_message("FIND THE RUNE KEY");
  audio_music(MUSIC_DUNGEON);
}

/* Start (or restart) the attract-mode demo behind the title screen: a fresh
 * random level the autopilot explores until it dies or finds the portal. */
static void demo_start(void){
  game_seed = rng_next();
  demo = true;
  autopilot = true;
  reset_stats();
  enter_level(1 + rng_range(4));
  fade = LIGHT_LEVELS;
}

/* Show the title screen, with the attract demo playing behind it. */
static void title_init(void){
  state = STATE_TITLE;
  state_t = 0;
  show_map = false;
  demo_start();
  audio_music(MUSIC_TITLE);
  hud_redraw();
}

/* ---- Player ---- */

static void try_move(int dir){
  int nx = player.cx + dir_dx[dir];
  int ny = player.cy + dir_dy[dir];
  struct Door* door = door_at(nx, ny);
  if (door != NULL && door->locked){
    if (g_stats.keys > 0){
      g_stats.keys -= 1;
      door->locked = 0;
      door_open(nx, ny);
      sfx(SFX_KEY);
      show_message("THE RUNE KEY BREAKS THE SEAL");
      hud_redraw();
    } else {
      sfx(SFX_LOCKED);
      show_message("SEALED - FIND THE RUNE KEY");
    }
    return;
  }
  if (door != NULL){
    door_open(nx, ny);
  } else if (cell_solid(nx, ny)){
    sfx(SFX_BUMP);
    return;
  }
  if (monster_at(nx, ny) != NULL){
    sfx(SFX_BUMP);
    return;
  }
  player.anim = 1;
  player.anim_t = 0;
  player.anim_len = STEP_TICKS;
  player.from_x = cell_center(player.cx);
  player.from_y = cell_center(player.cy);
  player.to_x = cell_center(nx);
  player.to_y = cell_center(ny);
  player.cx = nx;
  player.cy = ny;
  sfx(SFX_STEP);
}

/* Start a quarter turn: +1 clockwise (right), -1 counterclockwise (left). */
static void try_turn(int delta){
  player.anim = 2;
  player.anim_t = 0;
  player.anim_len = TURN_TICKS;
  player.from_angle = player.facing * ANGLE_QUARTER;
  player.to_angle = player.from_angle + delta * ANGLE_QUARTER;
  player.facing = (player.facing + delta) & 3;
}

/* Unit vector (16.16) through view column `col` at the current camera angle. */
static void aim_dir(int col, int* ux, int* uy){
  int dir_x = icos(camera.angle);
  int dir_y = isin(camera.angle);
  /* tangent of the column's angle off center: camX * plane length */
  int t = fixmul(((2 * col + 1 - VIEW_W) * FIX_ONE) / VIEW_W, PLANE_LEN);
  /* rotate (1, t) by the camera angle and scale by 1/sqrt(1 + t^2), using
   * 1 - t^2/2 + 3t^4/8 (|t| <= 0.7 keeps the error under 2%) */
  int t2 = fixmul(t, t);
  int inv = FIX_ONE - (t2 >> 1) + fixmul(fixmul(t2, t2), 24576);
  int x = dir_x - fixmul(t, dir_y);
  int y = dir_y + fixmul(t, dir_x);
  *ux = fixmul(x, inv);
  *uy = fixmul(y, inv);
}

/* Cast a spell toward view column `col`. */
static void cast(int kind, int col){
  if (player.cooldown > 0) return;
  int cost = kind == SHOT_FIRE ? FIRE_COST : SPARK_COST;
  if (g_stats.mp < cost){
    sfx(SFX_NOMANA);
    player.cooldown = 10;
    return;
  }
  g_stats.mp -= cost;
  int ux;
  int uy;
  aim_dir(col, &ux, &uy);
  int speed = kind == SHOT_FIRE ? FIRE_SPEED : SPARK_SPEED;
  int x = camera.x + (ux >> 2);
  int y = camera.y + (uy >> 2);
  spawn_shot(kind, x, y, fixmul(ux, speed), fixmul(uy, speed), true);
  player.cooldown = kind == SHOT_FIRE ? FIRE_COOLDOWN : SPARK_COOLDOWN;
  player.cast_t = 12;
  sfx(kind == SHOT_FIRE ? SFX_FIRE : SFX_SPARK);
  hud_redraw();
}

/* View column under the mouse crosshair (screen x / 4). */
static int cursor_col(void){
  int col = cursor_x >> 2;
  if (col >= VIEW_W) col = VIEW_W - 1;
  return col;
}

/* Begin the step or turn bound to `key`, relative to the current facing. */
static void start_command(int key){
  int f = player.facing;
  switch (key){
    case 'w': case 'W': case KEY_UP:    try_move(f); break;
    case 's': case 'S': case KEY_DOWN:  try_move((f + 2) & 3); break;
    case 'a': case 'A':                 try_move((f + 3) & 3); break;
    case 'd': case 'D':                 try_move((f + 1) & 3); break;
    case 'q': case 'Q': case KEY_LEFT:  try_turn(-1); break;
    case 'e': case 'E': case KEY_RIGHT: try_turn(1); break;
    default: break;
  }
}

/* Whether `key` is one of the step/turn keys (buffered while animating). */
static bool is_motion_key(int key){
  switch (key){
    case 'w': case 'W': case KEY_UP: case 's': case 'S': case KEY_DOWN:
    case 'a': case 'A': case 'd': case 'D': case 'q': case 'Q': case KEY_LEFT:
    case 'e': case 'E': case KEY_RIGHT:
      return true;
    default:
      return false;
  }
}

/* Advance the player one tick; true when a step just finished. */
static bool update_player(void){
  bool arrived = false;
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
      player.walk_phase += 1;
    } else {
      camera.angle = (player.from_angle +
                      fixmul(player.to_angle - player.from_angle, k)) & ANGLE_MASK;
    }
    if (player.anim_t >= player.anim_len){
      arrived = player.anim == 1;
      player.anim = 0;
      camera.x = cell_center(player.cx);
      camera.y = cell_center(player.cy);
      camera.angle = player.facing * ANGLE_QUARTER;
    }
  }
  if (player.cooldown > 0) player.cooldown -= 1;
  if (player.cast_t > 0) player.cast_t -= 1;
  player.regen += 1;
  if (player.regen >= MANA_REGEN_TICKS){
    player.regen = 0;
    if (g_stats.mp < MAX_MP){
      g_stats.mp += 1;
      if ((g_stats.mp & 3) == 0) hud_redraw();
    }
  }
  return arrived;
}

/* ---- Input ---- */

static void handle_key(int key){
  switch (state){
    case STATE_TITLE:
      if (key == '\n' || key == ' ') new_game();
      else if (key == 'h' || key == 'H'){ state = STATE_HELP; state_t = 0; }
      else if (key == 'x' || key == 'X') running = false;
      break;
    case STATE_HELP:
      state = STATE_TITLE;
      state_t = 0;
      break;
    case STATE_PLAY:
      if (is_motion_key(key)){
        if (player.anim == 0 && player.queued == 0) start_command(key);
        else player.queued = key;
      } else if (key == ' '){
        cast(SHOT_SPARK, VIEW_W / 2);
      } else if (key == 'f' || key == 'F'){
        cast(SHOT_FIRE, VIEW_W / 2);
      } else if (key == 'm' || key == 'M' || key == '\t'){
        show_map = !show_map;
      } else if (key == 'p' || key == 'P'){
        state = STATE_PAUSE;
        state_t = 0;
      } else if (key == '`'){
        show_fps = !show_fps;
      } else if (key == 'G'){
        /* cheat (shift+G): invulnerable */
        god_mode = !god_mode;
        show_message(god_mode ? "GOD MODE" : "MORTAL AGAIN");
      } else if (key == '='){
        /* cheat for the impatient: take the portal right now */
        g_stats.exit_reached = 1;
      } else if (key == '\\'){
        autopilot = !autopilot;
        show_message(autopilot ? "AUTOPILOT ON" : "AUTOPILOT OFF");
      }
      break;
    case STATE_PAUSE:
      if (key == 'x' || key == 'X'){
        title_init();
      } else {
        state = STATE_PLAY;
      }
      break;
    case STATE_DEAD:
    case STATE_WON:
      if (state_t > 60 && (key == '\n' || key == ' ')){
        final_score = g_stats.score;
        final_depth = g_stats.depth;
        entry_slot = score_rank(final_score);
        if (entry_slot >= 0){
          state = STATE_NAME;
          state_t = 0;
          entry_len = 0;
          entry_name[0] = 0;
        } else {
          title_init();
        }
      }
      break;
    case STATE_NAME:
      if (key == '\n' && entry_len > 0){
        while (entry_len < 3) entry_name[entry_len++] = ' ';
        entry_name[3] = 0;
        score_insert(entry_slot);
        title_init();
      } else if (key == KEY_BACKSPACE && entry_len > 0){
        entry_len -= 1;
        entry_name[entry_len] = 0;
      } else if (entry_len < 3 && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
                                   (key >= '0' && key <= '9'))){
        entry_name[entry_len++] = (key >= 'a' && key <= 'z') ? key - 32 : key;
        entry_name[entry_len] = 0;
      }
      break;
    default:
      break;
  }
}

/* Drain terminal key bytes and mouse events. Mouse motion moves the
 * crosshair within the view; a fresh button press casts toward it. */
static void poll_input(void){
  int avail = fd_bytes_available(STDIN);
  while (avail > 0){
    unsigned char byte = 0;
    if (read(STDIN, &byte, 1) != 1) break;
    avail -= 1;
    handle_key(byte);
  }
  int event;
  while ((event = getmouse()) != 0){
    cursor_x += MOUSE_DX(event);
    cursor_y += MOUSE_DY(event);
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > CURSOR_MAX_X) cursor_x = CURSOR_MAX_X;
    if (cursor_y > CURSOR_MAX_Y) cursor_y = CURSOR_MAX_Y;
    int buttons = MOUSE_BUTTONS(event);
    int pressed = buttons & ~mouse_buttons;
    mouse_buttons = buttons;
    if (state == STATE_PLAY){
      if (pressed & MOUSE_BUTTON_LEFT) cast(SHOT_SPARK, cursor_col());
      if (pressed & MOUSE_BUTTON_RIGHT) cast(SHOT_FIRE, cursor_col());
    }
  }
}

/* ---- Display ---- */

/* The crosshair (scale-0 sprite) and the staff (two scale-2 sprites) come
 * from assets.s as ready 12-bit colors. */
static void upload_sprite(int sprite, unsigned short* pixels){
  copy_words((void*)(g_spritemap + sprite * SPRITE_SHORTS), pixels, SPRITE_SHORTS / 2);
}

static bool staff_glowing;

/* Position the hardware-sprite overlays (staff and crosshair) for this frame:
 * the staff bobs with each step and thrusts when casting. */
static void place_overlays(void){
  bool glow = player.cast_t > 4 && state == STATE_PLAY;
  if (glow != staff_glowing){
    upload_sprite(STAFF_SPRITE_TOP, glow ? overlay_staff_glow : overlay_staff);
    staff_glowing = glow;
  }
  bool playing = state == STATE_PLAY || state == STATE_PAUSE;
  if (state == STATE_DEAD && state_t < 120){
    /* the staff slips from the fallen hero's hand */
    int y = STAFF_Y + state_t / 2 + (state_t * state_t) / 160;
    set_sprite_coords(STAFF_SPRITE_TOP, STAFF_X + state_t / 8, y);
    set_sprite_coords(STAFF_SPRITE_BOTTOM, STAFF_X + state_t / 8, y + 32);
    set_sprite_coords(CURSOR_SPRITE, SPRITE_HIDDEN, SPRITE_HIDDEN);
    return;
  }
  if (playing && !show_map){
    int bob = isin(player.walk_phase * 160) >> 14;
    if (bob < 0) bob = -bob;
    int thrust = player.cast_t > 6 ? 12 - player.cast_t : player.cast_t;
    int y = STAFF_Y + bob - thrust;
    set_sprite_coords(STAFF_SPRITE_TOP, STAFF_X, y);
    set_sprite_coords(STAFF_SPRITE_BOTTOM, STAFF_X, y + 32);
    /* scale-1 sprite: coordinates are in 2-pixel units */
    set_sprite_coords(CURSOR_SPRITE, (cursor_x >> 1) - 16, (cursor_y >> 1) - 16);
  } else {
    set_sprite_coords(STAFF_SPRITE_TOP, SPRITE_HIDDEN, SPRITE_HIDDEN);
    set_sprite_coords(STAFF_SPRITE_BOTTOM, SPRITE_HIDDEN, SPRITE_HIDDEN);
    set_sprite_coords(CURSOR_SPRITE, SPRITE_HIDDEN, SPRITE_HIDDEN);
  }
}

/* Take over the display: map the tile and sprite memory, switch the tile
 * layer to 4x, point the 20x12 view cells at tiles 0..239 column-major,
 * blank the rest, and set up the status-bar, staff, and crosshair sprites. */
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

  for (int i = 0; i < TILEMAP_SHORTS_PER_TILE; ++i){
    g_tilemap[BLANK_TILE * TILEMAP_SHORTS_PER_TILE + i] = 0;
  }
  for (int ty = 0; ty < TILE_FB_ROWS; ++ty){
    for (int tx = 0; tx < TILE_FB_COLS; ++tx){
      int tile = BLANK_TILE;
      if (tx < VIEW_TILE_COLS && ty < VIEW_TILE_ROWS){
        tile = tx * VIEW_TILE_ROWS + ty;
      }
      g_tile_fb[ty * TILE_FB_COLS + tx] = tile;
    }
  }
  for (int s = 0; s < 16; ++s){
    set_sprite_coords(s, SPRITE_HIDDEN, SPRITE_HIDDEN);
  }
  for (int s = 0; s < HUD_SPRITE_COUNT; ++s){
    int sprite = HUD_SPRITE_FIRST + s;
    set_sprite_scale(sprite, TILE_SCALE_4X);
    set_sprite_coords(sprite, s * 32, HUD_Y);
  }
  upload_sprite(STAFF_SPRITE_TOP, overlay_staff);
  upload_sprite(STAFF_SPRITE_BOTTOM, overlay_staff + SPRITE_SHORTS);
  upload_sprite(CURSOR_SPRITE, overlay_cursor);
  set_sprite_scale(STAFF_SPRITE_TOP, TILE_SCALE_4X);
  set_sprite_scale(STAFF_SPRITE_BOTTOM, TILE_SCALE_4X);
  set_sprite_scale(CURSOR_SPRITE, 1);
}

/* ---- Status bar ---- */

static void draw_bar(int x, int y, int w, int value, int max, unsigned short fg, unsigned short bg){
  fill_rect(SURF_HUD, x - 1, y - 1, w + 2, 6, 0x0000);
  fill_rect(SURF_HUD, x, y, w, 4, bg);
  int filled = value * w / max;
  fill_rect(SURF_HUD, x, y, filled, 4, fg);
  if (filled > 0) fill_rect(SURF_HUD, x, y, filled, 1, fg | 0x0444);
}

/* Redraw the whole status bar from the current stats and upload it. */
static void hud_redraw(void){
  char buf[16];
  fill_rect(SURF_HUD, 0, 0, HUD_W, HUD_H, 0x0211);
  fill_rect(SURF_HUD, 0, 0, HUD_W, 1, 0x027C);
  fill_rect(SURF_HUD, 0, 1, HUD_W, 1, 0x0136);
  if (state == STATE_TITLE || state == STATE_HELP){
    draw_text(SURF_HUD, 4, 6, "A DIOPTASE ADVENTURE", 0x0ACE);
    draw_text(SURF_HUD, 4, 14, "CPU, OS, COMPILER: ALL HOMEMADE", 0x0688);
    hud_upload();
    return;
  }
  draw_text(SURF_HUD, 3, 5, "HP", 0x066F);
  draw_bar(13, 5, 44, g_stats.hp, g_stats.max_hp, 0x022E, 0x0215);
  draw_text(SURF_HUD, 3, 14, "MP", 0x0F96);
  draw_bar(13, 14, 44, g_stats.mp, MAX_MP, 0x0E73, 0x0522);
  /* crystal count with a little gem */
  fill_rect(SURF_HUD, 66, 6, 3, 1, 0x0BFA);
  fill_rect(SURF_HUD, 65, 7, 5, 2, 0x07D3);
  fill_rect(SURF_HUD, 66, 9, 3, 1, 0x0480);
  fill_rect(SURF_HUD, 67, 10, 1, 1, 0x0480);
  draw_text(SURF_HUD, 72, 6, format_int(buf, g_stats.crystals), 0x0BFA);
  if (g_stats.keys > 0){
    fill_rect(SURF_HUD, 65, 15, 3, 3, 0x02CF);
    fill_rect(SURF_HUD, 68, 16, 5, 1, 0x02CF);
    fill_rect(SURF_HUD, 71, 17, 1, 1, 0x02CF);
    draw_text(SURF_HUD, 75, 15, "KEY", 0x02CF);
  }
  draw_text(SURF_HUD, 102, 6, "DEPTH", 0x0899);
  draw_text(SURF_HUD, 126, 6, format_int(buf, g_stats.depth), 0x0FFF);
  draw_text(SURF_HUD, 102, 15, format_int(buf, g_stats.score), 0x04DF);
  hud_upload();
}

/* ---- Overlays drawn into the view ---- */

static void draw_centered(int y, char* s, unsigned short color){
  draw_text_shadow(SURF_VIEW, (VIEW_W - text_width(s)) / 2, y, s, color);
}

/* Draw scaled 8x8-font text centered in the view, with a drop shadow. */
static void draw_big_centered(int y, char* s, unsigned short color, int scale){
  int x = (VIEW_W - big_text_width(s) * scale) / 2;
  draw_big_text(SURF_VIEW, x + 1, y + 1, s, 0x0000, scale);
  draw_big_text(SURF_VIEW, x, y, s, color, scale);
}

/* Overlay the explored map: 3x3 pixels per cell, doors in orange (sealed in
 * gold), items as colored dots, the player as a white dot plus heading. */
static void draw_automap(void){
  dim_view(g_view);
  int ox = (VIEW_W - MAP_W * 3) / 2;
  for (int cy = 0; cy < MAP_H; ++cy){
    for (int cx = 0; cx < MAP_W; ++cx){
      int c = cy * MAP_W + cx;
      if (!g_seen[c]) continue;
      int v = g_map[c];
      unsigned short color;
      if (v == CELL_EMPTY) color = 0x0333;
      else if (v >= CELL_DOOR_BASE) color = g_doors[v - CELL_DOOR_BASE].locked ? 0x00CF : 0x027B;
      else color = 0x0567;
      fill_rect(SURF_VIEW, ox + cx * 3, cy * 3, 3, 3, color);
    }
  }
  for (int i = 0; i < MAX_ITEMS; ++i){
    struct Item* it = &g_items[i];
    if (!it->active || !g_seen[it->cy * MAP_W + it->cx]) continue;
    unsigned short color = 0x066F;
    if (it->type == ITEM_CRYSTAL) color = 0x09F4;
    else if (it->type == ITEM_KEY) color = 0x02EF;
    else if (it->type == ITEM_PORTAL || it->type == ITEM_HEART) color = 0x0FF8;
    fill_rect(SURF_VIEW, ox + it->cx * 3 + 1, it->cy * 3 + 1, 1, 1, color);
  }
  int px = ox + player.cx * 3 + 1;
  int py = player.cy * 3 + 1;
  fill_rect(SURF_VIEW, px, py, 1, 1, 0x0FFF);
  fill_rect(SURF_VIEW, px + dir_dx[player.facing], py + dir_dy[player.facing], 1, 1, 0x0FFF);
  draw_text_shadow(SURF_VIEW, 2, 90, "MAP", 0x0ACE);
}

/* Title screen text over the darkened attract demo. */
static void draw_title(void){
  dim_view(g_view);
  draw_logo((VIEW_W - big_text_width("DELVE") * 3) / 2, 6, "DELVE", 3, state_t);
  draw_centered(36, "INTO THE DIOPTASE DEEP", 0x0ACE);
  draw_centered(50, "ENTER - DESCEND", (state_t & 32) ? 0x0FFF : 0x0BBB);
  draw_centered(57, "H - HOW TO PLAY    X - QUIT", 0x0999);
  draw_centered(67, "- DEEPEST DELVERS -", 0x02CF);
  char buf[12];
  for (int i = 0; i < 3; ++i){
    int y = 74 + i * 7;
    draw_text_shadow(SURF_VIEW, 46, y, score_names[i], 0x0FFF);
    format_int(buf, score_values[i]);
    draw_text_shadow(SURF_VIEW, 100 - text_width(buf), y, buf, 0x04DF);
    draw_text_shadow(SURF_VIEW, 106, y, "D", 0x0899);
    draw_text_shadow(SURF_VIEW, 110, y, format_int(buf, score_depths[i]), 0x0899);
  }
}

/* Controls and goals, over a doubly darkened demo. */
static void draw_help(void){
  dim_view(g_view);
  dim_view(g_view);
  draw_centered(3, "HOW TO PLAY", 0x02CF);
  draw_text_shadow(SURF_VIEW, 8, 13, "W S    STEP FORWARD / BACK", 0x0FFF);
  draw_text_shadow(SURF_VIEW, 8, 20, "A D    STEP LEFT / RIGHT", 0x0FFF);
  draw_text_shadow(SURF_VIEW, 8, 27, "Q E    TURN (OR ARROW KEYS)", 0x0FFF);
  draw_text_shadow(SURF_VIEW, 8, 36, "MOUSE  AIM", 0x0ACE);
  draw_text_shadow(SURF_VIEW, 8, 43, "LEFT   SPARK BOLT   (SPACE)", 0x0ACE);
  draw_text_shadow(SURF_VIEW, 8, 50, "RIGHT  FIREBALL     (F)", 0x0ACE);
  draw_text_shadow(SURF_VIEW, 8, 59, "M MAP  P PAUSE  (NOT ESC: IT", 0x0999);
  draw_text_shadow(SURF_VIEW, 8, 65, "               CLOSES THE EMULATOR)", 0x0777);
  draw_centered(73, "GATHER CRYSTALS. FIND THE RUNE KEY.", 0x09F4);
  draw_centered(80, "BREAK THE SEAL, ENTER THE PORTAL.", 0x09F4);
  draw_centered(88, "AT DEPTH 8 THE HEART AWAITS.", 0x0FF8);
}

/* In-game overlays: automap, boss health bar, messages, and the pause panel. */
static void draw_play_overlays(void){
  if (show_map){
    draw_automap();
  }
  int boss_hp;
  int boss_max;
  if (!show_map && world_boss(&boss_hp, &boss_max)){
    draw_centered(82, "THE WARDEN", 0x08F6);
    fill_rect(SURF_VIEW, 39, 89, 82, 5, 0x0000);
    fill_rect(SURF_VIEW, 40, 90, 80, 3, 0x0222);
    fill_rect(SURF_VIEW, 40, 90, boss_hp * 80 / boss_max, 3, 0x05E8);
  }
  if (message_t > 0){
    draw_centered(3, message, message_t > 30 || (message_t & 4) ? 0x0FFF : 0x0999);
  }
  if (state == STATE_PAUSE){
    dim_view(g_view);
    draw_big_centered(30, "PAUSED", 0x0FFF, 2);
    draw_centered(56, "ANY KEY - RESUME    X - GIVE UP", 0x0ACE);
  }
}

/* Victory or death summary with the run's statistics. */
static void draw_end_screen(bool won){
  char buf[12];
  if (won){
    draw_big_centered(8, "VICTORY", 0x04FF, 2);
    draw_centered(30, "THE HEART OF DIOPTASE IS YOURS", 0x09F4);
  } else {
    draw_big_centered(12, "YOU FELL", 0x044F, 2);
    draw_centered(34, "THE DEEP CLAIMS ANOTHER", 0x0999);
  }
  draw_text_shadow(SURF_VIEW, 44, 46, "SCORE", 0x0899);
  draw_text_shadow(SURF_VIEW, 84, 46, format_int(buf, g_stats.score), 0x04DF);
  draw_text_shadow(SURF_VIEW, 44, 53, "DEPTH", 0x0899);
  draw_text_shadow(SURF_VIEW, 84, 53, format_int(buf, g_stats.depth), 0x0FFF);
  draw_text_shadow(SURF_VIEW, 44, 60, "CRYSTALS", 0x0899);
  draw_text_shadow(SURF_VIEW, 84, 60, format_int(buf, g_stats.crystals), 0x09F4);
  draw_text_shadow(SURF_VIEW, 44, 67, "SLAIN", 0x0899);
  draw_text_shadow(SURF_VIEW, 84, 67, format_int(buf, g_stats.kills), 0x066F);
  if (state_t > 60 && (state_t & 32)){
    draw_centered(82, "PRESS ENTER", 0x0FFF);
  }
}

/* High-score initials entry with a blinking cursor. */
static void draw_name_entry(void){
  dim_view(g_view);
  draw_big_centered(12, "NEW RECORD", 0x02CF, 2);
  draw_centered(38, "ENTER YOUR INITIALS", 0x0ACE);
  char shown[4];
  for (int i = 0; i < 3; ++i){
    shown[i] = i < entry_len ? entry_name[i] : ((i == entry_len && (state_t & 16)) ? '_' : '.');
  }
  shown[3] = 0;
  draw_big_centered(50, shown, 0x0FFF, 3);
}

/* ---- Main loop ---- */

/* Let the autopilot press a key when the player is idle. */
static void run_autopilot(void){
  if (autopilot_wait > 0){
    autopilot_wait -= 1;
    return;
  }
  if (player.anim != 0 || player.queued != 0) return;
  int key = autopilot_decide(player.cx, player.cy, player.facing,
                             g_stats.keys > 0, g_stats.mp);
  if (key == ' '){
    cast(SHOT_SPARK, VIEW_W / 2);
  } else if (key == 'f'){
    cast(SHOT_FIRE, VIEW_W / 2);
  } else if (key != 0){
    start_command(key);
  }
  autopilot_wait = 6;
}

/* One tick of live gameplay (also drives the attract-mode demo). */
static void play_tick(void){
  if (fade > 0 && (state_t & 1)) fade -= 1;
  if (autopilot) run_autopilot();
  bool arrived = update_player();
  if (g_stats.hurt > 0) g_stats.hurt -= 1;
  if (g_stats.shake > 0) g_stats.shake -= 1;
  world_tick(player.cx, player.cy, camera.x, camera.y, arrived);
  if (arrived && !demo) hud_redraw();
}

/* Advance the whole game by one 60 Hz tick according to the current state. */
static void tick(void){
  state_t += 1;
  g_frame += 1;
  if (message_t > 0) message_t -= 1;
  switch (state){
    case STATE_TITLE:
    case STATE_HELP:
    case STATE_NAME:
      play_tick();
      if (g_stats.hp <= 0 || g_stats.exit_reached || state_t % 3600 == 0){
        demo_start();
      }
      break;
    case STATE_PLAY: {
      play_tick();
      if (g_stats.exit_reached == 1 && player.anim == 0){
        state = STATE_DESCEND;
        state_t = 0;
        sfx(SFX_PORTAL);
      } else if (g_stats.exit_reached == 2){
        g_stats.score += 1000 + g_stats.crystals * 25;
        state = STATE_WON;
        state_t = 0;
        audio_music(MUSIC_VICTORY);
        hud_redraw();
      }
      break;
    }
    case STATE_DESCEND:
      camera.angle = (camera.angle + (state_t >> 1)) & ANGLE_MASK;
      fade = state_t >> 2;
      if (state_t >= 64){
        g_stats.score += 100 * g_stats.depth;
        state = STATE_PLAY;
        state_t = 0;
        enter_level(g_stats.depth + 1);
        fade = LIGHT_LEVELS;
        static char buf[40];
        char num[12];
        copy_text(buf, "DEPTH ", 40);
        format_int(num, g_stats.depth);
        int n = 6;
        for (int i = 0; num[i]; ++i) buf[n++] = num[i];
        buf[n] = 0;
        show_message(g_stats.depth >= FINAL_DEPTH ? "THE WARDEN GUARDS THE HEART" : buf);
      }
      break;
    case STATE_DEAD:
      if (fade < 6 && (state_t & 7) == 0) fade += 1;
      world_tick(player.cx, player.cy, camera.x, camera.y, false);
      break;
    case STATE_WON:
      /* crystal fireworks bloom in front of the hero */
      camera.angle = (camera.angle + 3) & ANGLE_MASK;
      if ((state_t & 15) == 0){
        /* the hero may be standing in the Heart's one-cell alcove, so the
         * bursts start inside the camera's own cell, just ahead of it */
        static int colors[4] = {0x0BFA, 0x02DF, 0x0FE8, 0x0B5F};
        int ux = icos(camera.angle);
        int uy = isin(camera.angle);
        int side = (rng_range(5) - 2) * 0x1000;
        int x = camera.x + fixmul(ux, 0x6000) - fixmul(uy, side);
        int y = camera.y + fixmul(uy, 0x6000) + fixmul(ux, side);
        spawn_burst(x, y, 0xA000 + rng_range(0x3000), 14, colors[rng_range(4)], 0x0900);
        sfx(SFX_CRYSTAL);
      }
      world_tick(player.cx, player.cy, camera.x, camera.y, false);
      break;
    default:
      break;
  }
}

/* Render the 3D view plus the overlays for the current state into g_view. */
static void render_frame(void){
  struct Camera view = camera;
  if (g_stats.shake > 0 && state == STATE_PLAY){
    view.x += (rng_range(9) - 4) << 10;
    view.y += (rng_range(9) - 4) << 10;
  }
  set_light_offset(-fade);
  world_draw(g_frame);
  render_view(&view);
  if (state == STATE_PLAY && g_stats.hurt > 0){
    tint_view(g_view, 0x0FF0, 0x0008 + (g_stats.hurt >> 1));
  }
  switch (state){
    case STATE_TITLE: draw_title(); break;
    case STATE_HELP: draw_help(); break;
    case STATE_PLAY:
    case STATE_PAUSE: draw_play_overlays(); break;
    case STATE_DEAD:
      tint_view(g_view, 0x0FF0, 0x0006);
      draw_end_screen(false);
      break;
    case STATE_WON: draw_end_screen(true); break;
    case STATE_NAME: draw_name_entry(); break;
    default: break;
  }
}

/* Set up display, renderer, sound, and scores, then run the frame loop:
 * catch up on elapsed 60 Hz ticks, render, blit, and update the overlays. */
int main(void){
  display_init();
  render_init();
  audio_start();
  scores_load();
  title_init();

  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int fps_frames = 0;
  int fps = 0;
  char fps_text[16];
  while (running){
    unsigned now = get_vga_frame_counter();
    unsigned ticks = now - last;
    last = now;
    if (ticks > 4) ticks = 4;
    poll_input();
    for (unsigned t = 0; t < ticks && running; ++t){
      tick();
    }
    render_frame();
    fps_frames += 1;
    if (now - fps_mark >= 60){
      fps = fps_frames * 60 / (now - fps_mark);
      fps_frames = 0;
      fps_mark = now;
    }
    if (show_fps){
      int x = draw_text_shadow(SURF_VIEW, 2, 89, format_int(fps_text, fps), 0x00FF);
      x = draw_text_shadow(SURF_VIEW, x + 4, 89, format_int(fps_text, g_debug_bills), 0x0FF0);
      x = draw_text_shadow(SURF_VIEW, x + 4, 89, format_int(fps_text, g_debug_visible), 0x0F0F);
      draw_text_shadow(SURF_VIEW, x + 4, 89, format_int(fps_text, g_light_count), 0x0FFF);
    }
    blit_view(g_tilemap, g_view);
    place_overlays();
    hud_upload();
    audio_heartbeat();
    audio_mute_sfx(demo);
  }
  audio_stop();
  return 0;
}
