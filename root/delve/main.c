#include "delve.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"
#include "../crt/mouse.h"
#include "../crt/terminal.h"

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
 * Input: the terminal runs in raw keyboard mode while we play
 * (docs/terminal.md), forwarding every key press and release as a two-byte
 * record, so movement follows the keys actually held. Ctrl-C still reaches
 * the terminal, which leaves raw mode and terminates us; the shell turns raw
 * mode off after we exit in any case. The mouse stream is ours alone (the
 * terminal never reads it): its horizontal motion turns the view (mouse
 * look) and its real button state casts through the centered crosshair.
 *
 * Timing: game logic runs in 60 Hz ticks paced by the VGA frame counter,
 * which follows wall-clock time on both the emulator and hardware (PIT
 * jiffies count emulated cycles instead). Rendering takes whatever time is
 * left; on a slow frame several ticks run before the next render.
 */

volatile short* g_tilemap;
volatile short* g_tile_fb;
volatile short* g_spritemap;
volatile short* g_pixel_fb;
unsigned g_view_words[30720];
unsigned short* g_view;
unsigned g_frame;

/* Movement, per 60 Hz tick: speeds in cells (16.16), turning in angle units
 * (TURN_MAX in delve.h is the fastest). A held turn key starts at TURN_MIN
 * and speeds up by TURN_ACCEL per tick, so taps make fine adjustments. */
#define WALK_SPEED 0x0B00          /* ~2.6 cells per second */
#define RUN_SPEED 0x1280           /* ~4.3 cells per second */
#define TURN_MIN 12
#define TURN_ACCEL 3
#define STEP_DISTANCE 0xA000       /* walking distance per footstep sound */

/* Mouse look: horizontal mouse motion turns the view. Motion arrives in
 * screen pixels (640 across the window); each sensitivity level turns by a
 * number of eighths of an angle unit per pixel, so at the default a sweep
 * across the whole window turns about 180 degrees. There is no pointer grab
 * (the emulator cannot hold the host cursor under WSLg), so motion stops at
 * the window edge; the turn keys still work for big turns. */
#define MOUSE_SENS_LEVELS 9
#define MOUSE_SENS_DEFAULT 5       /* 1-based */
static int mouse_sens_eighths[MOUSE_SENS_LEVELS] = { 8, 12, 16, 20, 26, 32, 40, 52, 64 };

/* Spell timings (ticks) and speeds (cells per tick, 16.16); costs and the
 * rest of the spell rules are in delve.h. */
#define SPARK_COOLDOWN 10
#define SPARK_SPEED 0x7000
#define FIRE_COOLDOWN 45
#define FIRE_SPEED 0x2800
#define NOMANA_COOLDOWN 20

/* The crosshair is the only hardware sprite (scale 1: 2x2 screen pixels per
 * sprite pixel), fixed at the center of the view: spells fly straight ahead.
 * The staff is drawn into the view (render.c), resting with its top-left at
 * (STAFF_X, STAFF_Y) and its sleeve running off the bottom. */
#define CURSOR_SPRITE 0
#define STAFF_X 232
#define STAFF_Y 100

#define STATE_TITLE 0
#define STATE_PLAY 1
#define STATE_PAUSE 2
#define STATE_DEAD 3
#define STATE_WON 4
#define STATE_HELP 5
#define STATE_DESCEND 6
#define STATE_NAME 7

/* The player moves freely: a square of half-width PLAYER_RADIUS sliding
 * along walls (see move_player). The camera is the player's eye. */
struct Player {
  int x;           /* position, 16.16 cells */
  int y;
  int angle;       /* heading, 12-bit */
  int vx;          /* velocity, 16.16 cells per tick */
  int vy;
  int turn_rate;   /* current turn speed while a turn key is held */
  int cx;          /* the cell holding (x, y) */
  int cy;
  int cooldown;    /* ticks until the next spell */
  int cast_t;      /* staff thrust animation */
  int regen;       /* ticks toward the next point of mana */
  int regen_delay; /* ticks until mana starts coming back after a cast */
  int nomana_t;    /* ticks before another "no mana" message */
  int walk_phase;  /* staff bob phase, advanced by walking */
  int stride;      /* distance walked since the last footstep sound */
  int bump_t;      /* ticks before another bump or door message */
};

static struct Player player;
static struct Camera camera;
static bool running = true;
static int state = STATE_TITLE;
static int state_t;            /* ticks spent in the current state */
static bool demo;              /* attract mode: the autopilot plays behind the title */
static bool autopilot;         /* the autopilot drives the player (demo or '\\' key) */
static bool god_mode;          /* cheat: no damage */
static unsigned game_seed;
/* The dungeon seed from the command line (`delve seed=N`), or 0 for a new
 * random dungeon every game. */
static unsigned fixed_seed;
static bool show_map;
static bool show_fps;
static int fade;               /* light levels removed from the whole view */

/* Mouse: buttons held, horizontal motion not yet turned into a turn, and the
 * fraction of an angle unit left over from the last turn (in eighths). */
static int mouse_buttons;
static int mouse_dx;
static int look_frac;
static int mouse_sens = MOUSE_SENS_DEFAULT;

/* Keys held right now, by keycode (raw keyboard records, see poll_input). */
static bool key_down[256];
/* The first byte of a raw record whose second byte has not arrived yet. */
static int pending_code = -1;

static char message[40];
static int message_t;

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

/* World coordinate (16.16) of the center of cell index c along one axis. */
static int cell_center(int c){
  return (c << 16) + FIX_HALF;
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
  player.x = cell_center(sx);
  player.y = cell_center(sy);
  player.cx = sx;
  player.cy = sy;
  player.angle = sdir * ANGLE_QUARTER;
  player.vx = 0;
  player.vy = 0;
  player.turn_rate = 0;
  camera.x = player.x;
  camera.y = player.y;
  camera.angle = player.angle;
  autopilot_reset();
  world_tick(player.cx, player.cy, player.x, player.y, true);
  hud_redraw();
}

/* Fresh hero: full health and mana, no crystals, score, or kills. */
static void reset_stats(void){
  g_stats.hp = MAX_HP;
  g_stats.max_hp = MAX_HP;
  g_stats.mp = MAX_MP;
  g_stats.max_mp = MAX_MP;
  g_stats.crystals = 0;
  g_stats.score = 0;
  g_stats.kills = 0;
  g_stats.hurt = 0;
  g_stats.shake = 0;
  g_stats.spark_damage = 2;
  g_stats.fire_damage = 6;
  g_stats.splash_damage = 4;
  g_stats.regen_ticks = MANA_REGEN_TICKS;
  player.cooldown = 0;
  player.cast_t = 0;
  player.regen_delay = 0;
}

/* Start a real game at depth 1 with a new random dungeon seed. */
static void new_game(void){
  game_seed = fixed_seed != 0 ? fixed_seed
                              : rng_next() ^ (get_vga_frame_counter() * 2654435761u);
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
  parallel_autotune_begin();
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

/* The door the last blocked move pushed against, if any. */
static int push_cx = -1;
static int push_cy = -1;

/* Whether a player centered at (x, y) would overlap a wall, a door that is
 * not open far enough, or (moving closer) a monster. A door it would overlap
 * is remembered in push_cx/push_cy so walking into a door opens it. */
static bool player_blocked(int x, int y){
  int x0 = (x - PLAYER_RADIUS) >> 16;
  int x1 = (x + PLAYER_RADIUS) >> 16;
  int y0 = (y - PLAYER_RADIUS) >> 16;
  int y1 = (y + PLAYER_RADIUS) >> 16;
  bool blocked = false;
  for (int cy = y0; cy <= y1; ++cy){
    for (int cx = x0; cx <= x1; ++cx){
      if (!cell_solid(cx, cy)) continue;
      blocked = true;
      if (door_at(cx, cy) != NULL){
        push_cx = cx;
        push_cy = cy;
      }
    }
  }
  if (blocked) return true;
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (!m->alive) continue;
    int d = approx_dist(m->x - x, m->y - y);
    /* moving away from an overlapping monster is always allowed */
    if (d < 0x9000 && d < approx_dist(m->x - player.x, m->y - player.y)) return true;
  }
  return false;
}

/* Open the door the player walked into, or explain why it stays shut. */
static void push_door(int cx, int cy){
  struct Door* door = door_at(cx, cy);
  if (door == NULL) return;
  if (!door->locked){
    door_open(cx, cy);
    return;
  }
  if (g_stats.keys > 0){
    g_stats.keys -= 1;
    door->locked = 0;
    door_open(cx, cy);
    sfx(SFX_KEY);
    show_message("THE RUNE KEY BREAKS THE SEAL");
    hud_redraw();
  } else if (player.bump_t == 0){
    sfx(SFX_LOCKED);
    show_message("SEALED - FIND THE RUNE KEY");
    player.bump_t = 90;
  }
}

/* Move by the current velocity one axis at a time, so a blocked axis does
 * not stop motion along the other (the player slides along walls). */
static void move_player(void){
  push_cx = -1;
  bool bumped = false;
  int nx = player.x + player.vx;
  if (player_blocked(nx, player.y)){
    bumped = player.vx > WALK_SPEED / 2 || player.vx < -WALK_SPEED / 2;
    player.vx = 0;
  } else {
    player.x = nx;
  }
  int ny = player.y + player.vy;
  if (player_blocked(player.x, ny)){
    bumped = bumped || player.vy > WALK_SPEED / 2 || player.vy < -WALK_SPEED / 2;
    player.vy = 0;
  } else {
    player.y = ny;
  }
  if (push_cx >= 0){
    push_door(push_cx, push_cy);
  } else if (bumped && player.bump_t == 0){
    sfx(SFX_BUMP);
    player.bump_t = 30;
  }
}

/* Cast a spell straight ahead, through the crosshair. Without the mana for
 * it the staff fizzles, and the HUD says so now and then; a successful cast
 * holds off mana regeneration for MANA_REGEN_DELAY ticks. */
static void cast(int kind){
  if (player.cooldown > 0) return;
  int cost = kind == SHOT_FIRE ? FIRE_COST : SPARK_COST;
  if (g_stats.mp < cost){
    sfx(SFX_NOMANA);
    player.cooldown = NOMANA_COOLDOWN;
    if (player.nomana_t == 0){
      show_message(kind == SHOT_FIRE ? "NOT ENOUGH MANA FOR FIRE" : "OUT OF MANA - FALL BACK");
      player.nomana_t = 120;
    }
    return;
  }
  g_stats.mp -= cost;
  int ux = icos(camera.angle);
  int uy = isin(camera.angle);
  int speed = kind == SHOT_FIRE ? FIRE_SPEED : SPARK_SPEED;
  int x = camera.x + (ux >> 2);
  int y = camera.y + (uy >> 2);
  spawn_shot(kind, x, y, fixmul(ux, speed), fixmul(uy, speed), true);
  player.cooldown = kind == SHOT_FIRE ? FIRE_COOLDOWN : SPARK_COOLDOWN;
  player.cast_t = 12;
  player.regen_delay = MANA_REGEN_DELAY;
  sfx(kind == SHOT_FIRE ? SFX_FIRE : SFX_SPARK);
  hud_redraw();
}

/* Whether the key with this keycode is held. */
static bool held(int code){
  return key_down[code & 255];
}

/* Whether either Shift key is held. */
static bool shift_held(void){
  return held(KEY_LEFT_SHIFT) || held(KEY_RIGHT_SHIFT);
}

/* Turn for the mouse motion gathered since the last tick, carrying the
 * fraction of an angle unit over so slow motion still turns. */
static int mouse_turn(void){
  int eighths = mouse_dx * mouse_sens_eighths[mouse_sens - 1] + look_frac;
  mouse_dx = 0;
  int units = eighths >> 3;           /* floor, also for negative motion */
  look_frac = eighths - (units << 3);
  return units;
}

/* Read this tick's controls from the held keys and the mouse. Mouse motion
 * turns directly; a held turn key turns slowly at first and speeds up
 * (player.turn_rate). The mouse buttons cast through the crosshair. */
static void read_controls(struct Controls* c){
  c->forward = 0;
  c->strafe = 0;
  c->turn = mouse_turn();
  c->cast = 0;
  if (held('w') || held(KEY_UP)) c->forward += 1;
  if (held('s') || held(KEY_DOWN)) c->forward -= 1;
  if (held('d')) c->strafe += 1;
  if (held('a')) c->strafe -= 1;
  int turn = 0;
  if (held('e') || held(KEY_RIGHT)) turn += 1;
  if (held('q') || held(KEY_LEFT)) turn -= 1;
  if (turn == 0){
    player.turn_rate = 0;
  } else {
    player.turn_rate = player.turn_rate < TURN_MIN ? TURN_MIN : player.turn_rate + TURN_ACCEL;
    if (player.turn_rate > TURN_MAX) player.turn_rate = TURN_MAX;
    c->turn += turn > 0 ? player.turn_rate : -player.turn_rate;
  }
  c->run = shift_held();
  if (held(' ') || (mouse_buttons & MOUSE_BUTTON_LEFT)) c->cast = SHOT_SPARK + 1;
  else if (held('f') || (mouse_buttons & MOUSE_BUTTON_RIGHT)) c->cast = SHOT_FIRE + 1;
}

/* Advance the player one tick under controls `c`: turn, accelerate toward
 * the held direction, slide along walls, cast, bob the staff, and regenerate
 * mana. Returns true when the player entered a new cell. */
static bool update_player(struct Controls* c){
  player.angle = (player.angle + c->turn) & ANGLE_MASK;
  int speed = c->run ? RUN_SPEED : WALK_SPEED;
  if (c->forward != 0 && c->strafe != 0) speed = fixmul(speed, 46341);  /* 1/sqrt(2) */
  int fx = fixmul(icos(player.angle), speed);
  int fy = fixmul(isin(player.angle), speed);
  int tvx = 0;
  int tvy = 0;
  if (c->forward > 0){ tvx += fx; tvy += fy; }
  if (c->forward < 0){ tvx -= fx; tvy -= fy; }
  /* right is (-sin, cos): clockwise on the map, since y grows southward */
  if (c->strafe > 0){ tvx -= fy; tvy += fx; }
  if (c->strafe < 0){ tvx += fy; tvy -= fx; }
  /* ease toward the target velocity: a short start and stop, no ice */
  player.vx += (tvx - player.vx) / 3;
  player.vy += (tvy - player.vy) / 3;
  if (tvx == 0 && player.vx > -0x100 && player.vx < 0x100) player.vx = 0;
  if (tvy == 0 && player.vy > -0x100 && player.vy < 0x100) player.vy = 0;
  int old_x = player.x;
  int old_y = player.y;
  move_player();
  int moved = approx_dist(player.x - old_x, player.y - old_y);
  player.walk_phase += moved >> 4;
  player.stride += moved;
  if (player.stride >= STEP_DISTANCE){
    player.stride -= STEP_DISTANCE;
    sfx(SFX_STEP);
  }
  if (c->cast == SHOT_SPARK + 1) cast(SHOT_SPARK);
  else if (c->cast == SHOT_FIRE + 1) cast(SHOT_FIRE);
  camera.x = player.x;
  camera.y = player.y;
  camera.angle = player.angle;

  if (player.bump_t > 0) player.bump_t -= 1;
  if (player.cooldown > 0) player.cooldown -= 1;
  if (player.cast_t > 0) player.cast_t -= 1;
  if (player.nomana_t > 0) player.nomana_t -= 1;
  /* mana flows back only once the hero stops casting */
  if (player.regen_delay > 0){
    player.regen_delay -= 1;
    player.regen = 0;
  } else if (++player.regen >= g_stats.regen_ticks){
    player.regen = 0;
    if (g_stats.mp < g_stats.max_mp) g_stats.mp += 1;
  }
  int cx = player.x >> 16;
  int cy = player.y >> 16;
  bool entered = cx != player.cx || cy != player.cy;
  player.cx = cx;
  player.cy = cy;
  return entered;
}

/* Compass heading for the status bar: 0 east, 1 south, 2 west, 3 north. */
static int facing(void){
  return ((player.angle + ANGLE_QUARTER / 2) >> 10) & 3;
}

/* ---- Input ---- */

/* React to a key going down (menus, toggles, cheats). Movement and casting
 * read the held keys every tick instead (read_controls). `key` is a raw
 * keycode: lowercase letters, KEY_ENTER for Enter. */
static void handle_key(int key){
  bool shift = shift_held();
  switch (state){
    case STATE_TITLE:
      if (key == KEY_ENTER || key == ' ') new_game();
      else if (key == 'h'){ state = STATE_HELP; state_t = 0; }
      else if (key == 'x') running = false;
      break;
    case STATE_HELP:
      state = STATE_TITLE;
      state_t = 0;
      break;
    case STATE_PLAY:
      if (key == 'm' || key == '\t'){
        show_map = !show_map;
      } else if (key == 'p'){
        state = STATE_PAUSE;
        state_t = 0;
      } else if (key == '`'){
        show_fps = !show_fps;
      } else if (key == '['){
        parallel_toggle();
      } else if (key == 'g' && shift){
        /* cheat (shift+G): invulnerable */
        god_mode = !god_mode;
        show_message(god_mode ? "GOD MODE" : "MORTAL AGAIN");
      } else if (key == '='){
        /* cheat for the impatient: take the portal right now (the last
         * depth has no portal, only the Heart) */
        if (g_stats.depth < FINAL_DEPTH) g_stats.exit_reached = 1;
        else show_message("NO DEEPER - TAKE THE HEART");
      } else if (key == '\\'){
        autopilot = !autopilot;
        autopilot_reset();
        show_message(autopilot ? "AUTOPILOT ON" : "AUTOPILOT OFF");
      } else if (key == ',' || key == '.'){
        /* mouse-look sensitivity, one digit on the message line */
        mouse_sens += key == '.' ? 1 : -1;
        if (mouse_sens < 1) mouse_sens = 1;
        if (mouse_sens > MOUSE_SENS_LEVELS) mouse_sens = MOUSE_SENS_LEVELS;
        char text[16];
        copy_text(text, "MOUSE SPEED 0", sizeof(text));
        text[12] = '0' + mouse_sens;
        show_message(text);
      }
      break;
    case STATE_PAUSE:
      if (key == 'x'){
        title_init();
      } else if (key == '['){
        /* render-mode A/B on a frozen scene */
        parallel_toggle();
      } else if (key != KEY_LEFT_SHIFT && key != KEY_RIGHT_SHIFT){
        state = STATE_PLAY;
      }
      break;
    case STATE_DEAD:
    case STATE_WON:
      if (state_t > 60 && (key == KEY_ENTER || key == ' ')){
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
      if (key == KEY_ENTER && entry_len > 0){
        while (entry_len < 3) entry_name[entry_len++] = ' ';
        entry_name[3] = 0;
        score_insert(entry_slot);
        title_init();
      } else if (key == KEY_BACKSPACE && entry_len > 0){
        entry_len -= 1;
        entry_name[entry_len] = 0;
      } else if (entry_len < 3 && ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9'))){
        entry_name[entry_len++] = (key >= 'a' && key <= 'z') ? key - 32 : key;
        entry_name[entry_len] = 0;
      }
      break;
    default:
      break;
  }
}

/* Drain the terminal's raw key records and the mouse. A record is two bytes
 * (keycode, then TERMINAL_RAW_RELEASE or 0); a read may end between the two,
 * so the first byte waits in pending_code. Only a key's first press reaches
 * handle_key, so held keys never repeat menu actions. Horizontal mouse motion
 * collects in mouse_dx for the next tick's turn (read_controls) while the
 * hero is ours to steer; elsewhere it is dropped, so moving the mouse on a
 * menu does not swing the view once play starts. */
static void poll_input(void){
  unsigned char buf[32];
  int avail = fd_bytes_available(STDIN);
  while (avail > 0){
    int n = read(STDIN, buf, avail < 32 ? avail : 32);
    if (n <= 0) break;
    avail -= n;
    for (int i = 0; i < n; ++i){
      if (pending_code < 0){
        pending_code = buf[i];
        continue;
      }
      int code = pending_code;
      pending_code = -1;
      if (buf[i] == TERMINAL_RAW_RELEASE){
        key_down[code] = false;
      } else if (!key_down[code]){
        key_down[code] = true;
        handle_key(code);
      }
    }
  }
  int event;
  while ((event = getmouse()) != 0){
    mouse_dx += MOUSE_DX(event);
    mouse_buttons = MOUSE_BUTTONS(event);
  }
  if (state != STATE_PLAY || autopilot){
    mouse_dx = 0;
    look_frac = 0;
  }
}

/* Switch the terminal to raw keyboard mode and drop whatever input was
 * already queued: until the terminal acts on the request, keys arrive as
 * translated single bytes, and parsing those as records would misalign. */
static void keyboard_begin(void){
  puts(TERMINAL_RAW_KEYBOARD_ON);
  unsigned start = get_vga_frame_counter();
  while (get_vga_frame_counter() - start < 6) yield();
  unsigned char buf[32];
  int avail;
  while ((avail = fd_bytes_available(STDIN)) > 0){
    read(STDIN, buf, avail < 32 ? avail : 32);
  }
}

/* Leave raw keyboard mode before exiting. Records the terminal forwarded
 * before it processed the request are still queued, and would reach the
 * shell as stray characters, so keep discarding input for a few frames. */
static void keyboard_end(void){
  puts(TERMINAL_RAW_KEYBOARD_OFF);
  unsigned start = get_vga_frame_counter();
  unsigned char buf[32];
  while (get_vga_frame_counter() - start < 12){
    int avail = fd_bytes_available(STDIN);
    if (avail > 0) read(STDIN, buf, avail < 32 ? avail : 32);
    else yield();
  }
}

/* ---- Display ---- */

/* RGB332 tile color byte for a 12-bit 0x0BGR color (the tile layer expands
 * the byte back to 12 bits; docs/mem_map.md). */
static int rgb332(int c){
  int r = (c & 15) >> 1;
  int g = ((c >> 4) & 15) >> 1;
  int b = ((c >> 8) & 15) >> 2;
  return (r << 5) | (g << 2) | b;
}

/* Take over the display: map the tile, sprite, and pixel memory, set the tile
 * layer up for text overlays (scale 1), and load the crosshair sprite. The
 * pixel framebuffer mapping is checked before the terminal is cleared, so a
 * kernel without get_pixel_fb() gets a readable error instead of a hang. */
static bool display_init(void){
  g_pixel_fb = get_pixel_fb();
  if ((int)g_pixel_fb == -1){
    puts("delve: cannot map the pixel framebuffer (get_pixel_fb, trap 62, failed)\n");
    return false;
  }
  /* Clear the terminal and hide its cursor, then give the terminal time to
   * act on that before we start writing the tile framebuffer. */
  puts("\x1b[2J\x1b[?25l");
  sleep(30);

  g_tilemap = get_tilemap();
  g_tile_fb = get_tile_fb();
  g_spritemap = get_spritemap();

  set_tile_scale(TILE_SCALE_TEXT);
  set_hscroll(0);
  set_vscroll(0);
  tiles_init();
  for (int s = 0; s < 16; ++s){
    set_sprite_coords(s, SPRITE_HIDDEN, SPRITE_HIDDEN);
  }
  copy_words((void*)(g_spritemap + CURSOR_SPRITE * SPRITE_SHORTS), overlay_cursor, SPRITE_SHORTS / 2);
  set_sprite_scale(CURSOR_SPRITE, 1);
  return true;
}

/* Whether the automap covers the view this frame. */
static bool map_open(void){
  return show_map && (state == STATE_PLAY || state == STATE_PAUSE);
}

/* What the renderer draws over the scene this frame: the staff (bobbing as
 * the hero walks, thrusting and blazing while casting, slipping away on
 * death) and a recolor pass (dimmed menus, the damage flash, the death tint). */
static void view_extras(struct ViewExtras* e){
  e->staff_image = -1;
  e->staff_x = STAFF_X;
  e->staff_y = STAFF_Y;
  if (state == STATE_DEAD && state_t < 120){
    e->staff_image = 0;
    e->staff_x = STAFF_X + state_t / 4;
    e->staff_y = STAFF_Y + state_t + (state_t * state_t) / 80;
  } else if ((state == STATE_PLAY || state == STATE_PAUSE) && !map_open()){
    int bob = isin(player.walk_phase) >> 13;
    if (bob < 0) bob = -bob;
    int thrust = player.cast_t > 6 ? 12 - player.cast_t : player.cast_t;
    e->staff_image = player.cast_t > 4 && state == STATE_PLAY ? 1 : 0;
    e->staff_y = STAFF_Y + bob - thrust * 2;
  }
  e->shade_shift = -1;
  e->shade_and = 0x0777;
  e->shade_or = 0;
  if (state == STATE_HELP || map_open()){
    e->shade_shift = 2;
    e->shade_and = 0x0333;
  } else if (state == STATE_TITLE || state == STATE_NAME || state == STATE_PAUSE){
    e->shade_shift = 1;
  } else if (state == STATE_DEAD){
    e->shade_shift = 0;
    e->shade_and = 0x0FF0;
    e->shade_or = 0x0006;
  } else if (state == STATE_PLAY && g_stats.hurt > 0){
    e->shade_shift = 0;
    e->shade_and = 0x0FF0;
    e->shade_or = 0x0008 + (g_stats.hurt >> 1);
  }
}

/* Show the crosshair at the center of the view while playing. It is a
 * scale-1 sprite, so its coordinates are in 2-pixel screen units: the view's
 * center is screen pixel (VIEW_W, VIEW_H), and the sprite is 32 units wide. */
static void place_cursor(void){
  if (state == STATE_PLAY && !map_open()){
    set_sprite_coords(CURSOR_SPRITE, VIEW_W / 2 - 16, VIEW_H / 2 - 16);
  } else {
    set_sprite_coords(CURSOR_SPRITE, SPRITE_HIDDEN, SPRITE_HIDDEN);
  }
}

/* ---- Status bar ----
 * Pixels (background, bars, icons, minimap) live in g_hud and are redrawn
 * only when they change; the numbers and labels are tile text placed every
 * frame (hud_text) on tile rows 25 and 27, which lie over status-bar rows
 * 8..15 and 24..31. */

#define BAR_X 24                   /* health and mana bars: 88 x 6 pixels */
#define BAR_W 88
#define HP_BAR_Y 9
#define MP_BAR_Y 25
#define MINI_X 248                 /* minimap: MINI_COLS x MINI_ROWS cells */
#define MINI_Y 4
#define MINI_COLS 24
#define MINI_ROWS 11
#define MINI_CELL 3

/* The status bar's plain background, drawn once (HUD_PIXELS shorts). */
static unsigned hud_bg[6400];
static bool hud_bg_ready;

/* Draw the background panel into hud_bg: dark stone with a gold rule over a
 * bronze one along the top. */
static void hud_background(void){
  fill_rect(SURF_HUD, 0, 0, HUD_W, HUD_H, 0x0211);
  fill_rect(SURF_HUD, 0, 0, HUD_W, 1, 0x027C);
  fill_rect(SURF_HUD, 0, 1, HUD_W, 1, 0x0136);
  copy_words(hud_bg, g_hud, HUD_PIXELS / 2);
  hud_bg_ready = true;
}

/* One stat bar with a black frame and a lighter top edge. */
static void draw_bar(int y, int value, int max, unsigned short fg, unsigned short bg){
  fill_rect(SURF_HUD, BAR_X - 1, y - 1, BAR_W + 2, 8, 0x0000);
  fill_rect(SURF_HUD, BAR_X, y, BAR_W, 6, bg);
  if (value < 0) value = 0;
  int filled = value * BAR_W / max;
  fill_rect(SURF_HUD, BAR_X, y, filled, 6, fg);
  if (filled > 0) fill_rect(SURF_HUD, BAR_X, y, filled, 1, fg | 0x0444);
}

/* Minimap: explored cells around the hero, north up, items as dots, the hero
 * as a white dot with a heading tick. */
static void draw_minimap(void){
  fill_rect(SURF_HUD, MINI_X - 1, MINI_Y - 1, MINI_COLS * MINI_CELL + 2, MINI_ROWS * MINI_CELL + 2, 0x0000);
  int cx0 = player.cx - MINI_COLS / 2;
  int cy0 = player.cy - MINI_ROWS / 2;
  for (int r = 0; r < MINI_ROWS; ++r){
    int cy = cy0 + r;
    if (cy < 0 || cy >= MAP_H) continue;
    for (int k = 0; k < MINI_COLS; ++k){
      int cx = cx0 + k;
      if (cx < 0 || cx >= MAP_W) continue;
      int c = cy * MAP_W + cx;
      if (!g_seen[c]) continue;
      int v = g_map[c];
      unsigned short color = 0x0567;
      if (v == CELL_EMPTY) color = 0x0333;
      else if (v >= CELL_DOOR_BASE) color = g_doors[v - CELL_DOOR_BASE].locked ? 0x00CF : 0x027B;
      fill_rect(SURF_HUD, MINI_X + k * MINI_CELL, MINI_Y + r * MINI_CELL, MINI_CELL, MINI_CELL, color);
    }
  }
  for (int i = 0; i < MAX_ITEMS; ++i){
    struct Item* it = &g_items[i];
    int k = it->cx - cx0;
    int r = it->cy - cy0;
    if (!it->active || k < 0 || r < 0 || k >= MINI_COLS || r >= MINI_ROWS) continue;
    if (!g_seen[it->cy * MAP_W + it->cx]) continue;
    unsigned short color = 0x066F;
    if (it->type == ITEM_CRYSTAL) color = 0x09F4;
    else if (it->type == ITEM_KEY) color = 0x02EF;
    else if (it->type == ITEM_PORTAL || it->type == ITEM_HEART) color = 0x0FF8;
    fill_rect(SURF_HUD, MINI_X + k * MINI_CELL + 1, MINI_Y + r * MINI_CELL + 1, 1, 1, color);
  }
  /* the hero at its true position within the cell grid */
  int px = MINI_X + (((player.x - (cx0 << 16)) >> 8) * MINI_CELL >> 8);
  int py = MINI_Y + (((player.y - (cy0 << 16)) >> 8) * MINI_CELL >> 8);
  fill_rect(SURF_HUD, px - 1, py - 1, 2, 2, 0x0FFF);
  fill_rect(SURF_HUD, px + (icos(player.angle) * 3 >> 16) - 1,
            py + (isin(player.angle) * 3 >> 16) - 1, 1, 1, 0x02CF);
}

/* What the status bar's pixels show of the stats: both bars' filled widths
 * and whether the rune key is held, packed so play_tick() can redraw the
 * bar whenever any of them changes (pickups, regeneration, damage). */
static int hud_shown = -1;

static int hud_state(void){
  if (g_stats.max_hp <= 0 || g_stats.max_mp <= 0) return 0;   /* before the first game */
  int hp = g_stats.hp < 0 ? 0 : g_stats.hp;
  return (hp * BAR_W / g_stats.max_hp) | ((g_stats.mp * BAR_W / g_stats.max_mp) << 8) |
         ((g_stats.keys > 0) << 16);
}

/* Redraw the status bar's pixels from the current stats (the upload happens
 * once per frame in hud_upload). */
static void hud_redraw(void){
  if (!hud_bg_ready) hud_background();
  copy_words(g_hud, hud_bg, HUD_PIXELS / 2);
  hud_touch();
  hud_shown = hud_state();
  /* these screens write their own lines of text across the whole panel */
  if (state == STATE_TITLE || state == STATE_HELP || state == STATE_NAME) return;
  /* dividers between the stat bars, the counters, and the minimap */
  fill_rect(SURF_HUD, 138, 5, 1, 31, 0x0136);
  fill_rect(SURF_HUD, 243, 5, 1, 31, 0x0136);
  draw_bar(HP_BAR_Y, g_stats.hp, g_stats.max_hp, 0x022E, 0x0215);
  draw_bar(MP_BAR_Y, g_stats.mp, g_stats.max_mp, 0x0E73, 0x0522);
  /* a little gem beside the crystal count */
  fill_rect(SURF_HUD, 143, 9, 5, 1, 0x0BFA);
  fill_rect(SURF_HUD, 142, 10, 7, 3, 0x07D3);
  fill_rect(SURF_HUD, 143, 13, 5, 1, 0x0480);
  fill_rect(SURF_HUD, 144, 14, 3, 1, 0x0480);
  fill_rect(SURF_HUD, 145, 15, 1, 1, 0x0480);
  if (g_stats.keys > 0){
    /* the rune key: a ring and a toothed shaft */
    fill_rect(SURF_HUD, 142, 26, 4, 4, 0x02CF);
    fill_rect(SURF_HUD, 143, 27, 2, 2, 0x0211);
    fill_rect(SURF_HUD, 146, 27, 5, 2, 0x02CF);
    fill_rect(SURF_HUD, 149, 29, 1, 2, 0x02CF);
  }
  draw_minimap();
}

/* The status bar's numbers and labels for this frame (tile text). */
static void hud_text(void){
  if (state == STATE_TITLE || state == STATE_HELP || state == STATE_NAME){
    tile_text(1, 25, "A DIOPTASE ADVENTURE", rgb332(0x0ACE));
    tile_text(1, 27, "CPU, OS, COMPILER: ALL HOMEMADE", rgb332(0x0688));
    return;
  }
  char buf[12];
  tile_text(1, 25, "HP", rgb332(0x066F));
  tile_text_right(17, 25, format_int(buf, g_stats.hp < 0 ? 0 : g_stats.hp), rgb332(0x0CCF));
  tile_text(1, 27, "MP", rgb332(0x0F96));
  tile_text_right(17, 27, format_int(buf, g_stats.mp), rgb332(0x0FCB));
  tile_text(19, 25, format_int(buf, g_stats.crystals), rgb332(0x0BFA));
  if (g_stats.keys > 0) tile_text(19, 27, "KEY", rgb332(0x02CF));
  tile_text(23, 25, "DEPTH", rgb332(0x0899));
  tile_text(29, 25, format_int(buf, g_stats.depth), rgb332(0x0FFF));
  tile_text_right(30, 27, format_int(buf, g_stats.score), rgb332(0x04DF));
}

/* ---- Screens ---- */

/* Overlay the explored map on the (dimmed) view: 5x5 pixels per cell, doors
 * in orange (sealed in gold), items as dots, the hero as a white dot with a
 * heading tick. Drawn into the back buffer by the game process. */
static void draw_automap(void){
  int ox = (VIEW_W - MAP_W * 5) / 2;
  int oy = (VIEW_H - MAP_H * 5) / 2;
  /* a thin frame around the map area */
  fill_rect(SURF_VIEW, ox - 2, oy - 2, MAP_W * 5 + 4, 1, 0x0136);
  fill_rect(SURF_VIEW, ox - 2, oy + MAP_H * 5 + 1, MAP_W * 5 + 4, 1, 0x0136);
  fill_rect(SURF_VIEW, ox - 2, oy - 2, 1, MAP_H * 5 + 4, 0x0136);
  fill_rect(SURF_VIEW, ox + MAP_W * 5 + 1, oy - 2, 1, MAP_H * 5 + 4, 0x0136);
  for (int cy = 0; cy < MAP_H; ++cy){
    for (int cx = 0; cx < MAP_W; ++cx){
      int c = cy * MAP_W + cx;
      if (!g_seen[c]) continue;
      int v = g_map[c];
      unsigned short color;
      if (v == CELL_EMPTY) color = 0x0333;
      else if (v >= CELL_DOOR_BASE) color = g_doors[v - CELL_DOOR_BASE].locked ? 0x00CF : 0x027B;
      else color = 0x0567;
      fill_rect(SURF_VIEW, ox + cx * 5, oy + cy * 5, 5, 5, color);
    }
  }
  for (int i = 0; i < MAX_ITEMS; ++i){
    struct Item* it = &g_items[i];
    if (!it->active || !g_seen[it->cy * MAP_W + it->cx]) continue;
    unsigned short color = 0x066F;
    if (it->type == ITEM_CRYSTAL) color = 0x09F4;
    else if (it->type == ITEM_KEY) color = 0x02EF;
    else if (it->type == ITEM_PORTAL || it->type == ITEM_HEART) color = 0x0FF8;
    fill_rect(SURF_VIEW, ox + it->cx * 5 + 1, oy + it->cy * 5 + 1, 3, 3, color);
  }
  int px = ox + ((player.x >> 8) * 5 >> 8);
  int py = oy + ((player.y >> 8) * 5 >> 8);
  fill_rect(SURF_VIEW, px - 1, py - 1, 3, 3, 0x0FFF);
  fill_rect(SURF_VIEW, px + (icos(player.angle) * 4 >> 16) - 1,
            py + (isin(player.angle) * 4 >> 16) - 1, 2, 2, 0x02CF);
}

/* Title screen over the dimmed attract demo: the logo with its sweeping
 * glint, the menu, and the best three scores. */
static void title_text(void){
  banner_set(0, "DELVE", 4, true, 0);
  banner_glint(0, state_t % (banner_width(0) + 90) - 30);
  banner_place(0, 2);
  tile_text_centered(8, "INTO THE DIOPTASE DEEP", rgb332(0x0ACE));
  tile_text_centered(11, "ENTER - DESCEND", rgb332((state_t & 32) ? 0x0FFF : 0x0BBB));
  tile_text_centered(13, "H - HOW TO PLAY    X - QUIT", rgb332(0x0999));
  tile_text_centered(16, "- DEEPEST DELVERS -", rgb332(0x02CF));
  char buf[12];
  for (int i = 0; i < 3; ++i){
    int row = 18 + i;
    tile_text(13, row, score_names[i], rgb332(0x0FFF));
    tile_text_right(24, row, format_int(buf, score_values[i]), rgb332(0x04DF));
    tile_text(25, row, "D", rgb332(0x0899));
    tile_text(26, row, format_int(buf, score_depths[i]), rgb332(0x0899));
  }
}

/* Controls, the two spells' trade-offs, and goals over the darkened demo. */
static void help_text(void){
  int white = rgb332(0x0FFF);
  int blue = rgb332(0x0ACE);
  int spark = rgb332(0x0FE8);
  int fire = rgb332(0x03AF);
  tile_text_centered(1, "HOW TO PLAY", rgb332(0x02CF));
  tile_text(3, 3, "W S A D   WALK AND SIDESTEP", white);
  tile_text(3, 4, "MOUSE     TURN (OR Q E, ARROWS)", white);
  tile_text(3, 5, "SHIFT     RUN", white);
  tile_text(3, 7, "LEFT      SPARK     6 MANA (SPACE)", blue);
  tile_text(3, 8, "RIGHT     FIREBALL 30 MANA (F)", blue);
  tile_text_centered(10, "SPARKS ARE CHEAP AND QUICK BUT WEAK.", spark);
  tile_text_centered(11, "WRAITHS SHRUG THEM OFF.", spark);
  tile_text_centered(12, "FIREBALLS BLAST AND STUN A WHOLE PACK", fire);
  tile_text_centered(13, "AND SHATTER CRACKED WALLS - BUT THEY", fire);
  tile_text_centered(14, "BURN YOU TOO IF YOU STAND TOO CLOSE.", fire);
  tile_text_centered(15, "MANA RETURNS ONCE YOU STOP CASTING.", blue);
  tile_text_centered(17, "M MAP  P PAUSE  , . MOUSE SPEED  ` FPS", rgb332(0x0999));
  tile_text_centered(18, "(ESC CLOSES THE EMULATOR)", rgb332(0x0777));
  tile_text_centered(20, "GATHER CRYSTALS. FIND THE RUNE KEY.", rgb332(0x09F4));
  tile_text_centered(21, "BREAK THE SEAL, ENTER THE PORTAL.", rgb332(0x09F4));
  tile_text_centered(22, "AT DEPTH 8 THE HEART AWAITS.", rgb332(0x0FF8));
}

/* In-game text: the message line, the boss's health, the automap label, and
 * the pause panel. */
static void play_text(void){
  if (message_t > 0){
    tile_text_centered(2, message, rgb332(message_t > 30 || (message_t & 4) ? 0x0FFF : 0x0999));
  }
  int boss_hp;
  int boss_max;
  if (!map_open() && world_boss(&boss_hp, &boss_max)){
    tile_text_centered(20, "THE WARDEN", rgb332(0x08F6));
    tile_bar(10, 21, 160, boss_hp * 160 / boss_max, rgb332(0x05E8));
  }
  if (map_open()) tile_text(1, 22, "MAP", rgb332(0x0ACE));
  if (state == STATE_PAUSE){
    banner_set(0, "PAUSED", 3, false, 0x0FFF);
    banner_place(0, 7);
    tile_text_centered(13, "ANY KEY - RESUME    X - GIVE UP", rgb332(0x0ACE));
  }
}

/* Victory or death summary with the run's statistics. */
static void end_text(bool won){
  if (won){
    banner_set(0, "VICTORY", 3, false, 0x04FF);
    banner_place(0, 2);
    tile_text_centered(7, "THE HEART OF DIOPTASE IS YOURS", rgb332(0x09F4));
  } else {
    banner_set(0, "YOU FELL", 3, false, 0x044F);
    banner_place(0, 2);
    tile_text_centered(7, "THE DEEP CLAIMS ANOTHER", rgb332(0x0999));
  }
  char buf[12];
  int label = rgb332(0x0899);
  tile_text(12, 10, "SCORE", label);
  tile_text_right(28, 10, format_int(buf, g_stats.score), rgb332(0x04DF));
  tile_text(12, 11, "DEPTH", label);
  tile_text_right(28, 11, format_int(buf, g_stats.depth), rgb332(0x0FFF));
  tile_text(12, 12, "CRYSTALS", label);
  tile_text_right(28, 12, format_int(buf, g_stats.crystals), rgb332(0x09F4));
  tile_text(12, 13, "SLAIN", label);
  tile_text_right(28, 13, format_int(buf, g_stats.kills), rgb332(0x066F));
  if (won){
    tile_text_centered(16, "PLAYED ON DIOPTASE: HOMEMADE CPU,", rgb332(0x0688));
    tile_text_centered(17, "OS, COMPILER, AND ASSEMBLER", rgb332(0x0688));
  }
  if (state_t > 60 && (state_t & 32)){
    tile_text_centered(20, "PRESS ENTER", rgb332(0x0FFF));
  }
}

/* High-score initials entry with a blinking cursor. */
static void name_text(void){
  banner_set(0, "NEW RECORD", 2, false, 0x02CF);
  banner_place(0, 3);
  tile_text_centered(7, "ENTER YOUR INITIALS", rgb332(0x0ACE));
  char shown[4];
  for (int i = 0; i < 3; ++i){
    shown[i] = i < entry_len ? entry_name[i] : ((i == entry_len && (state_t & 16)) ? '_' : '.');
  }
  shown[3] = 0;
  banner_set(1, shown, 4, false, 0x0FFF);
  banner_place(1, 10);
}

/* All of this frame's tile overlays: the state's screen text, the status
 * bar's numbers, and the frame-rate readout. */
static void place_text(int fps){
  text_begin();
  switch (state){
    case STATE_TITLE: title_text(); break;
    case STATE_HELP: help_text(); break;
    case STATE_PLAY:
    case STATE_PAUSE: play_text(); break;
    case STATE_DEAD: end_text(false); break;
    case STATE_WON: end_text(true); break;
    case STATE_NAME: name_text(); break;
    default: break;
  }
  hud_text();
  if (show_fps){
    /* frame rate, billboards queued / visible, lights, render processes,
     * and the game process's share of the 40 column groups */
    char line[40];
    char num[12];
    int n = 0;
    int values[6] = {fps, g_debug_bills, g_debug_visible, g_light_count, parallel_workers() + 1,
                     parallel_game_groups()};
    for (int i = 0; i < 6; ++i){
      format_int(num, values[i]);
      for (int k = 0; num[k]; ++k) line[n++] = num[k];
      line[n++] = ' ';
    }
    line[n - 1] = 0;
    tile_text(0, 23, line, rgb332(0x00FF));
  }
  text_end();
}

/* ---- Main loop ---- */

/* Ticks until the next ambient water drip. */
static int drip_wait = 300;

/* One tick of live gameplay (also drives the attract-mode demo). */
static void play_tick(void){
  drip_wait -= 1;
  if (drip_wait <= 0){
    sfx(SFX_DRIP);
    drip_wait = 240 + rng_range(600);
  }
  if (fade > 0 && (state_t & 1)) fade -= 1;
  struct Controls controls;
  if (autopilot){
    autopilot_steer(player.x, player.y, player.angle, g_stats.keys > 0, g_stats.mp, &controls);
  } else {
    read_controls(&controls);
  }
  int old_facing = facing();
  bool entered = update_player(&controls);
  if (g_stats.hurt > 0) g_stats.hurt -= 1;
  if (g_stats.shake > 0) g_stats.shake -= 1;
  world_tick(player.cx, player.cy, player.x, player.y, entered);
  if (!demo && (facing() != old_facing || hud_state() != hud_shown)) hud_redraw();
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
      if (g_stats.exit_reached == 1){
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
        if (g_stats.depth == FINAL_DEPTH) audio_music(MUSIC_BOSS);
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

/* Render the 3D view with this frame's extras and show it. The automap is
 * the one overlay drawn into the back buffer, so with it open the game
 * process draws it over the finished view and copies the frame out itself;
 * otherwise each render process copies its own strip. */
static void render_frame(void){
  struct Camera view = camera;
  if (g_stats.shake > 0 && state == STATE_PLAY){
    view.x += (rng_range(9) - 4) << 10;
    view.y += (rng_range(9) - 4) << 10;
  }
  set_light_offset(-fade);
  world_draw(g_frame, view.x, view.y);
  struct ViewExtras extras;
  view_extras(&extras);
  bool overlays = map_open();
  parallel_render(&view, &extras, overlays);
  if (overlays){
    draw_automap();
    blit_strip(g_pixel_fb, g_view, 0, VIEW_GROUPS);
  }
}

/* Read the command line options: `seed=N` (decimal) makes every game descend
 * through the same dungeon, for sharing a run or for repeatable tests;
 * `sens=N` (1..9) sets the mouse-look sensitivity (, and . change it in play). */
static void parse_args(int argc, char** argv){
  for (int i = 1; i < argc; ++i){
    char* a = argv[i];
    if (a[0] == 's' && a[1] == 'e' && a[2] == 'e' && a[3] == 'd' && a[4] == '='){
      int pos = 5;
      fixed_seed = (unsigned)parse_int(a, &pos);
    } else if (a[0] == 's' && a[1] == 'e' && a[2] == 'n' && a[3] == 's' && a[4] == '='){
      int pos = 5;
      int level = parse_int(a, &pos);
      if (level >= 1 && level <= MOUSE_SENS_LEVELS) mouse_sens = level;
    }
  }
}

/* Set up display, renderer, sound, and scores, then run the frame loop:
 * catch up on elapsed 60 Hz ticks, render and show the view, then update the
 * tile overlays, crosshair, and status bar. */
int main(int argc, char** argv){
  /* the render workers run at high priority; so must we, or a worker that
   * lands on our core would preempt the strip we render ourselves */
  request_priority(DIOPTASE_PRIORITY_HIGH);
  parse_args(argc, argv);
  g_view = (unsigned short*)g_view_words;
  world_init();
  hud_init();
  if (!display_init()) return 1;
  keyboard_begin();
  render_init();
  audio_start();
  parallel_start();
  /* after the forks, so the children start unpinned and pin themselves */
  pin_core(CORE_GAME);
  scores_load();
  title_init();

  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int fps_frames = 0;
  int fps = 0;
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
    place_text(fps);
    place_cursor();
    hud_upload();
    audio_heartbeat();
    audio_mute_sfx(demo);
  }
  parallel_stop();
  audio_stop();
  keyboard_end();
  return 0;
}
