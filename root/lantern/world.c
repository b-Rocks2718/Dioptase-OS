#include "lantern.h"
#include "../crt/string.h"

int cells[MAP_H][MAP_W];
struct Actor actors[MAX_ACTORS];
int actor_count;

int player_x;
int player_y;
int player_angle;
int player_hp;
int player_moving;
int cores_left;
int cores_total;
int exit_x;
int exit_y;
int hurt_timer;
int world_dirty;
int shot_flash;
unsigned lantern_time;
char message[80];
unsigned message_until;
static int shot_wait;
static int step_wait;

/* The vault, one row per string. Doors and the gate start closed. */
static char* map_rows[24] = {
  "################################",
  "#%%%%%%%%%%%%%%%%%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%...C...%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%....S..%%++++++++++++#",
  "#%%%%%%%%%.......%%........++++#",
  "#.......%%.......%%.....H..++++#",
  "#.......%%%%%DD%%%%...C....++++#",
  "#.................#........++++#",
  "#.@............S..D....S...G.E+#",
  "#.....H...........D........G..+#",
  "#.................#........++++#",
  "#.......%%%%%DD%%%%..SC....++++#",
  "#%%%%%%%%%.......%%........++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%....S..%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "#%%%%%%%%%.....H.%%++++++++++++#",
  "#%%%%%%%%%.......%%++++++++++++#",
  "################################"
};

static int spawn_count;

/* Absolute value that stays in range for a 32-bit cell delta. */
static int iabs(int v) {
  if (v < 0) {
    return -v;
  }
  return v;
}

/* True when a cell blocks walking, sight, and shots. */
static int cell_blocks(int cell) {
  return cell == CELL_STONE || cell == CELL_BRICK || cell == CELL_METAL ||
         cell == CELL_DOOR || cell == CELL_GATE;
}

/* Cell under a fixed-point position, or stone when the point leaves the map. */
static int cell_at(int x, int y) {
  int cx = x >> 8;
  int cy = y >> 8;
  if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) {
    return CELL_STONE;
  }
  return cells[cy][cx];
}

/* True when a player-sized box around the point does not intersect a wall. */
static int can_stand(int x, int y) {
  int r = 48;
  if (cell_blocks(cell_at(x - r, y - r))) return 0;
  if (cell_blocks(cell_at(x + r, y - r))) return 0;
  if (cell_blocks(cell_at(x - r, y + r))) return 0;
  if (cell_blocks(cell_at(x + r, y + r))) return 0;
  return 1;
}

/* Slide along a wall instead of stopping when only one axis is blocked. */
static void try_move(int dx, int dy) {
  if (can_stand(player_x + dx, player_y)) {
    player_x += dx;
  }
  if (can_stand(player_x, player_y + dy)) {
    player_y += dy;
  }
}

/* Step from an actor toward the player and stop at the first solid cell. */
static int sees_player(struct Actor* actor) {
  int x = actor->x;
  int y = actor->y;
  int dx = player_x - x;
  int dy = player_y - y;
  int steps = (iabs(dx) + iabs(dy)) >> 7;
  int i;
  if (steps < 1) {
    return 1;
  }
  if (steps > 32) {
    steps = 32;
  }
  dx /= steps;
  dy /= steps;
  for (i = 0; i < steps; i++) {
    x += dx;
    y += dy;
    if (cell_blocks(cell_at(x, y))) {
      return 0;
    }
  }
  return 1;
}

/* Place one actor on a cell center. Extra spawns past MAX_ACTORS are dropped. */
static void add_actor(int x, int y, int kind, int hp) {
  struct Actor* actor;
  if (actor_count >= MAX_ACTORS) {
    return;
  }
  actor = &actors[actor_count];
  actor->x = (x << 8) + 128;
  actor->y = (y << 8) + 128;
  actor->kind = kind;
  actor->hp = hp;
  actor->alive = 1;
  actor->hurt = 0;
  actor->aggro = 0;
  actor->cooldown = spawn_count * 5;
  actor_count++;
  spawn_count++;
}

/* Turn every gate cell into floor once the last core is gone. */
static void open_gates(void) {
  int y;
  int x;
  for (y = 0; y < MAP_H; y++) {
    for (x = 0; x < MAP_W; x++) {
      if (cells[y][x] == CELL_GATE) {
        cells[y][x] = CELL_FLOOR;
      }
    }
  }
  world_dirty = 1;
  set_message("The gate opens.", 180);
  sfx_play(SFX_CORE);
}

/* Apply a hit. Cores open the gate when the last one breaks. */
static void hurt_actor(struct Actor* actor) {
  actor->hp--;
  actor->hurt = 8;
  actor->aggro = 1;
  if (actor->hp > 0) {
    sfx_play(SFX_HIT);
    return;
  }
  actor->alive = 0;
  if (actor->kind == KIND_CORE) {
    cores_left--;
    if (cores_left <= 0) {
      open_gates();
    } else if (cores_left == 1) {
      set_message("A core shatters. One remains.", 150);
      sfx_play(SFX_CORE);
    } else {
      set_message("A core shatters.", 120);
      sfx_play(SFX_CORE);
    }
    return;
  }
  sfx_play(SFX_HIT);
}

/* Walk a light bolt forward and damage the first actor or wall it meets. */
static void fire_shot(void) {
  int x = player_x;
  int y = player_y;
  int sx = lantern_dir_x >> 2;
  int sy = lantern_dir_y >> 2;
  int step;
  int i;

  shot_flash = 4;
  sfx_play(SFX_SHOT);
  if (sx == 0 && sy == 0) {
    sx = 64;
  }

  for (step = 0; step < 48; step++) {
    x += sx;
    y += sy;
    for (i = 0; i < actor_count; i++) {
      struct Actor* actor = &actors[i];
      int dx;
      int dy;
      if (!actor->alive) continue;
      if (actor->kind == KIND_HEALTH) continue;
      dx = actor->x - x;
      dy = actor->y - y;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (dx < 140 && dy < 140) {
        hurt_actor(actor);
        return;
      }
    }
    if (cell_blocks(cell_at(x, y))) {
      return;
    }
  }
}

/* Open the door the player is facing, or explain why the gate stays shut. */
static void try_use(void) {
  int fx = lantern_dir_x;
  int fy = lantern_dir_y;
  int best_x = -1;
  int best_y = -1;
  int best_dot = 64;
  int dy;
  int dx;

  for (dy = -1; dy <= 1; dy++) {
    for (dx = -1; dx <= 1; dx++) {
      int cx = (player_x >> 8) + dx;
      int cy = (player_y >> 8) + dy;
      int cell;
      int mx;
      int my;
      int dot;
      if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) continue;
      cell = cells[cy][cx];
      if (cell != CELL_DOOR && cell != CELL_GATE) continue;
      mx = ((cx << 8) + 128) - player_x;
      my = ((cy << 8) + 128) - player_y;
      dot = (mx * fx + my * fy) >> 8;
      if (dot > best_dot) {
        best_dot = dot;
        best_x = cx;
        best_y = cy;
      }
    }
  }

  if (best_x < 0) {
    return;
  }
  if (cells[best_y][best_x] == CELL_GATE) {
    set_message("The cores still seal this gate.", 120);
    return;
  }
  cells[best_y][best_x] = CELL_FLOOR;
  world_dirty = 1;
  set_message("The door opens.", 90);
  sfx_play(SFX_DOOR);
}

/* Move one alert sentry and hurt the player when it gets close. */
static void update_sentry(struct Actor* actor, int dt) {
  int dx = player_x - actor->x;
  int dy = player_y - actor->y;
  int adx = iabs(dx);
  int ady = iabs(dy);
  int dist = adx + ady;
  int step;
  int denom;
  int mx;
  int my;
  int nx;
  int ny;

  if (!actor->aggro) {
    if ((dist >> 8) <= 8 && sees_player(actor)) {
      actor->aggro = 1;
    } else {
      return;
    }
  }

  step = 8 * dt;
  if (step > 24) step = 24;
  denom = adx > ady ? adx : ady;
  if (denom < 1) denom = 1;
  mx = dx * step / denom;
  my = dy * step / denom;
  nx = actor->x + mx;
  ny = actor->y + my;
  if (!cell_blocks(cell_at(nx, actor->y))) {
    actor->x = nx;
  }
  if (!cell_blocks(cell_at(actor->x, ny))) {
    actor->y = ny;
  }

  if (actor->cooldown > 0) {
    actor->cooldown -= dt;
  }
  if (dist < 220 && actor->cooldown <= 0) {
    player_hp -= 12;
    if (player_hp < 0) player_hp = 0;
    hurt_timer = 10;
    actor->cooldown = 22;
    sfx_play(SFX_HURT);
    set_message("The sentry strikes.", 50);
  }
}

/* Collect a pickup when the player walks into it. */
static void touch_pickups(void) {
  int i;
  for (i = 0; i < actor_count; i++) {
    struct Actor* actor = &actors[i];
    int dx;
    int dy;
    if (!actor->alive || actor->kind != KIND_HEALTH) continue;
    dx = actor->x - player_x;
    dy = actor->y - player_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < 160 && dy < 160) {
      actor->alive = 0;
      player_hp += 35;
      if (player_hp > 100) player_hp = 100;
      sfx_play(SFX_PICKUP);
      set_message("The lantern steadies.", 90);
    }
  }
}

void set_message(char* text, unsigned frames) {
  int n = 0;
  while (text[n] != 0 && n < 79) {
    message[n] = text[n];
    n++;
  }
  message[n] = 0;
  message_until = lantern_time + frames;
}

int load_level(void) {
  int y;
  int x;

  for (y = 0; y < MAP_H; y++) {
    if (strlen(map_rows[y]) != MAP_W) return -1;
  }

  actor_count = 0;
  spawn_count = 0;
  cores_left = 0;
  cores_total = 0;
  player_hp = 100;
  player_angle = 0;
  player_moving = 0;
  hurt_timer = 0;
  shot_flash = 0;
  shot_wait = 0;
  step_wait = 0;
  world_dirty = 1;
  exit_x = 1;
  exit_y = 1;

  for (y = 0; y < MAP_H; y++) {
    for (x = 0; x < MAP_W; x++) {
      char tile = map_rows[y][x];
      int cell = CELL_STONE;
      if (tile == '%' ) cell = CELL_BRICK;
      else if (tile == '+') cell = CELL_METAL;
      else if (tile == '.' || tile == '@' || tile == 'S' || tile == 'C' || tile == 'H') cell = CELL_FLOOR;
      else if (tile == 'D') cell = CELL_DOOR;
      else if (tile == 'G') cell = CELL_GATE;
      else if (tile == 'E') cell = CELL_EXIT;
      cells[y][x] = cell;
      if (tile == '@') {
        player_x = (x << 8) + 128;
        player_y = (y << 8) + 128;
      } else if (tile == 'S') {
        add_actor(x, y, KIND_SENTRY, 3);
      } else if (tile == 'C') {
        add_actor(x, y, KIND_CORE, 4);
        cores_left++;
        cores_total++;
      } else if (tile == 'H') {
        add_actor(x, y, KIND_HEALTH, 1);
      } else if (tile == 'E') {
        exit_x = x;
        exit_y = y;
      }
    }
  }
  return 0;
}

int update_world(int dt, int forward, int back, int left, int right, int turn, int use, int fire) {
  int fx;
  int fy;
  int rx;
  int ry;
  int mx = 0;
  int my = 0;
  int step;
  int i;

  if (dt < 1) dt = 1;
  player_angle = (player_angle + turn) & (ANGLE_FULL - 1);
  render_sync_facing();
  fx = lantern_dir_x;
  fy = lantern_dir_y;
  rx = lantern_right_x;
  ry = lantern_right_y;

  if (forward) {
    mx += fx;
    my += fy;
  }
  if (back) {
    mx -= fx;
    my -= fy;
  }
  if (right) {
    mx += rx;
    my += ry;
  }
  if (left) {
    mx -= rx;
    my -= ry;
  }

  player_moving = (mx != 0 || my != 0);
  step = 16 * dt;
  if (step > 40) step = 40;
  if (player_moving) {
    try_move((mx * step) >> 8, (my * step) >> 8);
    step_wait -= dt;
    if (step_wait <= 0) {
      sfx_play(SFX_STEP);
      step_wait = 16;
    }
  }

  if (use) {
    try_use();
  }

  if (shot_wait > 0) {
    shot_wait -= dt;
  }
  if (fire && shot_wait <= 0) {
    fire_shot();
    shot_wait = 14;
  }

  for (i = 0; i < actor_count; i++) {
    struct Actor* actor = &actors[i];
    if (!actor->alive) continue;
    if (actor->hurt > 0) {
      actor->hurt -= dt;
      if (actor->hurt < 0) actor->hurt = 0;
    }
    if (actor->kind == KIND_SENTRY) {
      update_sentry(actor, dt);
    } else if (actor->kind == KIND_CORE) {
      int dx = player_x - actor->x;
      int dy = player_y - actor->y;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (actor->cooldown > 0) actor->cooldown -= dt;
      if (dx < 180 && dy < 180 && actor->cooldown <= 0) {
        player_hp -= 8;
        if (player_hp < 0) player_hp = 0;
        hurt_timer = 10;
        actor->cooldown = 28;
        sfx_play(SFX_HURT);
        set_message("The core burns.", 50);
      }
    }
  }

  touch_pickups();

  if (player_hp <= 0) {
    player_hp = 0;
    sfx_play(SFX_DIE);
    set_message("The lantern goes out. R tries again.", 240);
    return STATE_DEAD;
  }

  if ((player_x >> 8) == exit_x && (player_y >> 8) == exit_y && cores_left <= 0) {
    sfx_play(SFX_WIN);
    set_message("The vault lets you leave. R plays again.", 240);
    return STATE_WON;
  }
  return STATE_PLAY;
}
