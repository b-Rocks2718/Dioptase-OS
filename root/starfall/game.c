#include "starfall.h"

#define FIRE_RANGE 26
#define SHOT_SPEED 184
#define PLAYER_INVULN_FRAMES 48
#define MOVE_LATCH_FRAMES 11
#define PLAYER_MOVE_FRAMES 3

/* Keep the procedural sequence independent from any library RNG. */
static unsigned next_random(struct Game* game) {
  unsigned value = game->rng;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  game->rng = value;
  return value;
}

/* Take the absolute value of bounded grid offsets. */
static int magnitude(int value) {
  return value < 0 ? -value : value;
}

/* Return whether a position is inside the traversable part of the field. */
static int open_cell(struct Game* game, int x, int y) {
  if (x < 1 || y < 1 || x >= FIELD_W - 1 || y >= FIELD_H - 1) {
    return 0;
  }
  return game->cells[y * FIELD_W + x] == CELL_SPACE;
}

/* Reset bounded object pools when a new wave begins. */
static void clear_objects(struct Game* game) {
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    game->enemies[i].active = 0;
  }
  for (int i = 0; i < MAX_SHOTS; ++i) {
    game->shots[i].active = 0;
  }
  for (int i = 0; i < MAX_DROPS; ++i) {
    game->drops[i].active = 0;
  }
}

/* Scatter asteroid clusters while keeping the spawn rim and center clear. */
static void make_field(struct Game* game) {
  for (int y = 0; y < FIELD_H; ++y) {
    for (int x = 0; x < FIELD_W; ++x) {
      int index = y * FIELD_W + x;
      game->cells[index] = (x == 0 || y == 0 ||
                            x == FIELD_W - 1 || y == FIELD_H - 1)
                             ? CELL_ROCK : CELL_SPACE;
      game->stars[index] = (unsigned char)(next_random(game) % 23 == 0 ? 1 :
                                           next_random(game) % 67 == 0 ? 2 : 0);
    }
  }

  for (int group = 0; group < 22; ++group) {
    int x = 4 + (int)(next_random(game) % (FIELD_W - 8));
    int y = 4 + (int)(next_random(game) % (FIELD_H - 8));
    int length = 1 + (int)(next_random(game) % 3);
    int horizontal = (int)(next_random(game) & 1);
    for (int n = 0; n < length; ++n) {
      int px = x + (horizontal ? n : 0);
      int py = y + (horizontal ? 0 : n);
      if (magnitude(px - FIELD_W / 2) > 5 ||
          magnitude(py - FIELD_H / 2) > 5) {
        game->cells[py * FIELD_W + px] = CELL_ROCK;
      }
    }
  }
}

/* Enter a fresh wave with the same player upgrades and remaining hull. */
static void begin_wave(struct Game* game) {
  make_field(game);
  clear_objects(game);
  game->phase = PHASE_PLAY;
  game->wave_frame = 0;
  game->spawned = 0;
  game->spawn_budget = game->wave == FINAL_WAVE ? 15 : 10 + game->wave * 3;
  game->player_x = FIELD_W / 2;
  game->player_y = FIELD_H / 2;
  game->hurt_timer = 0;
  game->move_timer = 0;
  game->move_latch = 0;
  game->fire_timer = 0;
  game->pulse_flash = 0;
  game->sound_events |= SOUND_WAVE;
}

/* Start a full run while preserving the recorded best score. */
static void reset_run(struct Game* game) {
  game->score = 0;
  game->wave = 1;
  game->kills = 0;
  game->max_hp = 6;
  game->hp = 6;
  game->pulse_charge = PULSE_FULL;
  game->rapid_level = 0;
  game->twin_level = 0;
  game->hull_level = 0;
  game->emp_level = 0;
  game->magnet_level = 0;
  begin_wave(game);
}

/* Initialize a repeatable title starfield and remember the prior best. */
void game_init(struct Game* game, unsigned seed, unsigned best) {
  game->rng = seed == 0 ? 0x53A7F411u : seed;
  game->frame = 0;
  game->score = 0;
  game->best = best;
  game->phase = PHASE_TITLE;
  game->wave = 0;
  game->wave_frame = 0;
  game->spawn_budget = 0;
  game->spawned = 0;
  game->kills = 0;
  game->hp = 0;
  game->max_hp = 0;
  game->hurt_timer = 0;
  game->move_timer = 0;
  game->move_x = 0;
  game->move_y = 0;
  game->move_latch = 0;
  game->fire_timer = 0;
  game->pulse_charge = 0;
  game->pulse_flash = 0;
  game->rapid_level = 0;
  game->twin_level = 0;
  game->hull_level = 0;
  game->emp_level = 0;
  game->magnet_level = 0;
  for (int i = 0; i < 3; ++i) game->upgrade_choices[i] = 0;
  game->sound_events = 0;
  game->player_x = FIELD_W / 2;
  game->player_y = FIELD_H / 2;
  make_field(game);
  clear_objects(game);
}

/* Find a living enemy whose center covers this cell, including the core body. */
static int enemy_at(struct Game* game, int x, int y, int except) {
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    struct Enemy* enemy = &game->enemies[i];
    if (!enemy->active || i == except) {
      continue;
    }
    int radius = enemy->kind == ENEMY_CORE ? 1 : 0;
    if (magnitude(x - enemy->x) <= radius &&
        magnitude(y - enemy->y) <= radius) {
      return i;
    }
  }
  return -1;
}

/* Count remaining opponents for wave completion and the status display. */
int game_enemy_count(struct Game* game) {
  int count = 0;
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (game->enemies[i].active) {
      ++count;
    }
  }
  return count;
}

/* Place one opponent on a clear edge cell so the player has warning space. */
static int spawn_enemy(struct Game* game, int kind) {
  int slot = -1;
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (!game->enemies[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return 0;
  }

  int x = 0;
  int y = 0;
  for (int attempt = 0; attempt < 32; ++attempt) {
    unsigned edge = next_random(game) % 4;
    x = 2 + (int)(next_random(game) % (FIELD_W - 4));
    y = 2 + (int)(next_random(game) % (FIELD_H - 4));
    if (edge == 0) x = 2;
    if (edge == 1) x = FIELD_W - 3;
    if (edge == 2) y = 2;
    if (edge == 3) y = FIELD_H - 3;
    if (kind == ENEMY_CORE) {
      x = FIELD_W / 2;
      y = 3;
    }
    if (open_cell(game, x, y) && enemy_at(game, x, y, -1) < 0 &&
        magnitude(x - game->player_x) + magnitude(y - game->player_y) > 11) {
      break;
    }
  }
  if (!open_cell(game, x, y) || enemy_at(game, x, y, -1) >= 0) {
    return 0;
  }

  struct Enemy* enemy = &game->enemies[slot];
  enemy->active = 1;
  enemy->kind = kind;
  enemy->x = x;
  enemy->y = y;
  enemy->hp = kind == ENEMY_HUNTER ? 1 :
              kind == ENEMY_SNIPER ? 2 :
              kind == ENEMY_BRUTE ? 4 : 36;
  enemy->move_timer = (int)(next_random(game) % 12);
  enemy->fire_timer = 30 + (int)(next_random(game) % 24);
  return 1;
}

/* Create a short-lived energy drop at a defeated enemy's last position. */
static void spawn_drop(struct Game* game, int x, int y) {
  for (int i = 0; i < MAX_DROPS; ++i) {
    if (!game->drops[i].active) {
      game->drops[i].active = 1;
      game->drops[i].x = x;
      game->drops[i].y = y;
      return;
    }
  }
}

/* Award a kill and free the enemy slot immediately for later spawns. */
static void damage_enemy(struct Game* game, int index, int damage) {
  struct Enemy* enemy = &game->enemies[index];
  if (!enemy->active) {
    return;
  }
  enemy->hp -= damage;
  if (enemy->hp > 0) {
    return;
  }
  enemy->active = 0;
  game->kills++;
  game->score += enemy->kind == ENEMY_CORE ? 500 :
                 enemy->kind == ENEMY_BRUTE ? 45 :
                 enemy->kind == ENEMY_SNIPER ? 30 : 20;
  if (game->score > game->best) {
    game->best = game->score;
  }
  if (enemy->kind == ENEMY_CORE || next_random(game) % 3 == 0) {
    spawn_drop(game, enemy->x, enemy->y);
  }
  game->sound_events |= SOUND_KILL;
}

/* Hurt the player once per recovery window and end the run at zero hull. */
static void damage_player(struct Game* game) {
  if (game->hurt_timer > 0 || game->phase != PHASE_PLAY) {
    return;
  }
  game->hp--;
  game->hurt_timer = PLAYER_INVULN_FRAMES;
  game->sound_events |= SOUND_DAMAGE;
  if (game->hp <= 0) {
    game->hp = 0;
    game->phase = PHASE_DEAD;
  }
}

/* Put an aimed shot in the first unused slot; no allocation occurs in combat. */
static void create_shot(struct Game* game, int side, int x, int y,
                        int target_x, int target_y, int vx_adjust,
                        int vy_adjust) {
  int dx = target_x - x;
  int dy = target_y - y;
  int scale = magnitude(dx) > magnitude(dy) ? magnitude(dx) : magnitude(dy);
  if (scale == 0) {
    return;
  }
  for (int i = 0; i < MAX_SHOTS; ++i) {
    struct Shot* shot = &game->shots[i];
    if (!shot->active) {
      shot->active = 1;
      shot->side = side;
      shot->fx = x * 256 + 128;
      shot->fy = y * 256 + 128;
      shot->vx = dx * SHOT_SPEED / scale + vx_adjust;
      shot->vy = dy * SHOT_SPEED / scale + vy_adjust;
      shot->damage = 1;
      shot->pierce = side == SHOT_PLAYER && game->twin_level >= 2 ? 1 : 0;
      shot->ttl = 48;
      return;
    }
  }
}

/* Find the nearest target within the weapon's automatic acquisition range. */
static int nearest_enemy(struct Game* game) {
  int nearest = -1;
  int distance = FIRE_RANGE + 1;
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (!game->enemies[i].active) {
      continue;
    }
    int dx = game->enemies[i].x - game->player_x;
    int dy = game->enemies[i].y - game->player_y;
    int candidate = magnitude(dx) + magnitude(dy);
    if (candidate < distance) {
      nearest = i;
      distance = candidate;
    }
  }
  return nearest;
}

/* Fire the pulse blaster automatically so one hand can steer the ship. */
static void fire_player_weapon(struct Game* game) {
  if (game->fire_timer > 0) {
    game->fire_timer--;
    return;
  }
  int target = nearest_enemy(game);
  if (target < 0) {
    return;
  }
  struct Enemy* enemy = &game->enemies[target];
  int dx = enemy->x - game->player_x;
  int dy = enemy->y - game->player_y;
  create_shot(game, SHOT_PLAYER, game->player_x, game->player_y,
              enemy->x, enemy->y, 0, 0);
  if (game->twin_level > 0) {
    create_shot(game, SHOT_PLAYER, game->player_x, game->player_y,
                enemy->x, enemy->y, -dy * 7, dx * 7);
    create_shot(game, SHOT_PLAYER, game->player_x, game->player_y,
                enemy->x, enemy->y, dy * 7, -dx * 7);
  }
  game->fire_timer = 10 - game->rapid_level * 2;
  if (game->fire_timer < 4) {
    game->fire_timer = 4;
  }
  game->sound_events |= SOUND_FIRE;
}

/* Consume EMP charge to clear bullets and damage nearby opponents. */
static void trigger_pulse(struct Game* game) {
  if (game->pulse_charge < PULSE_FULL) {
    return;
  }
  int radius = 6 + game->emp_level;
  game->pulse_charge = 0;
  game->pulse_flash = 12;
  game->sound_events |= SOUND_PULSE;
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (game->enemies[i].active &&
        magnitude(game->enemies[i].x - game->player_x) +
        magnitude(game->enemies[i].y - game->player_y) <= radius) {
      damage_enemy(game, i, 3 + game->emp_level);
    }
  }
  for (int i = 0; i < MAX_SHOTS; ++i) {
    if (game->shots[i].active && game->shots[i].side == SHOT_ENEMY &&
        magnitude((game->shots[i].fx >> 8) - game->player_x) +
        magnitude((game->shots[i].fy >> 8) - game->player_y) <= radius + 2) {
      game->shots[i].active = 0;
    }
  }
}

/* Step the ship at a fixed cadence and stop after key repeat goes quiet. */
static void move_player(struct Game* game, struct GameInput input) {
  if (input.move_pressed) {
    game->move_x = input.move_x;
    game->move_y = input.move_y;
    game->move_latch = MOVE_LATCH_FRAMES;
  } else if (game->move_latch > 0) {
    game->move_latch--;
  }
  if (game->move_timer > 0) {
    game->move_timer--;
  }
  if (game->move_latch == 0 || game->move_timer > 0) {
    return;
  }
  game->move_timer = PLAYER_MOVE_FRAMES;
  int nx = game->player_x + game->move_x;
  int ny = game->player_y + game->move_y;
  if (!open_cell(game, nx, ny)) {
    return;
  }
  if (enemy_at(game, nx, ny, -1) >= 0) {
    damage_player(game);
    return;
  }
  game->player_x = nx;
  game->player_y = ny;
}

/* Advance all bullets and resolve the first collision along each path. */
static void move_shots(struct Game* game) {
  for (int i = 0; i < MAX_SHOTS; ++i) {
    struct Shot* shot = &game->shots[i];
    if (!shot->active) {
      continue;
    }
    shot->fx += shot->vx;
    shot->fy += shot->vy;
    int x = shot->fx >> 8;
    int y = shot->fy >> 8;
    shot->ttl--;
    if (shot->ttl <= 0 || !open_cell(game, x, y)) {
      shot->active = 0;
      continue;
    }
    if (shot->side == SHOT_PLAYER) {
      int target = enemy_at(game, x, y, -1);
      if (target >= 0) {
        damage_enemy(game, target, shot->damage);
        if (shot->pierce > 0) {
          shot->pierce--;
        } else {
          shot->active = 0;
        }
      }
    } else if (x == game->player_x && y == game->player_y) {
      shot->active = 0;
      damage_player(game);
    }
  }
}

/* Test whether an enemy can occupy a cell without crossing a wall or ally. */
static int enemy_can_move(struct Game* game, int self, int x, int y) {
  if (!open_cell(game, x, y) || enemy_at(game, x, y, self) >= 0) {
    return 0;
  }
  if (game->enemies[self].kind == ENEMY_CORE) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (!open_cell(game, x + dx, y + dy)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

/* Move toward the player, trying the other axis when a rock blocks the path. */
static void pursue_player(struct Game* game, int index) {
  struct Enemy* enemy = &game->enemies[index];
  int dx = game->player_x - enemy->x;
  int dy = game->player_y - enemy->y;
  if (magnitude(dx) + magnitude(dy) <= 1) {
    damage_player(game);
    return;
  }
  if (enemy->kind == ENEMY_SNIPER &&
      magnitude(dx) + magnitude(dy) <= 7) {
    return;
  }
  int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0;
  int sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
  int x_first = magnitude(dx) >= magnitude(dy);
  if (next_random(game) % 5 == 0) {
    x_first = !x_first;
  }
  int nx = enemy->x + (x_first ? sx : 0);
  int ny = enemy->y + (x_first ? 0 : sy);
  if ((nx == game->player_x && ny == game->player_y) ||
      !enemy_can_move(game, index, nx, ny)) {
    nx = enemy->x + (x_first ? 0 : sx);
    ny = enemy->y + (x_first ? sy : 0);
  }
  if (!(nx == game->player_x && ny == game->player_y) &&
      enemy_can_move(game, index, nx, ny)) {
    enemy->x = nx;
    enemy->y = ny;
  }
}

/* Give the core an eight-way salvo that forces the player to maneuver. */
static void fire_core_salvo(struct Game* game, struct Enemy* enemy) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx != 0 || dy != 0) {
        create_shot(game, SHOT_ENEMY, enemy->x, enemy->y,
                    enemy->x + dx * 8, enemy->y + dy * 8, 0, 0);
      }
    }
  }
}

/* Run each opponent's movement and weapon clocks once per frame. */
static void move_enemies(struct Game* game) {
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    struct Enemy* enemy = &game->enemies[i];
    if (!enemy->active) {
      continue;
    }
    if (enemy->move_timer > 0) {
      enemy->move_timer--;
    } else {
      pursue_player(game, i);
      enemy->move_timer = enemy->kind == ENEMY_HUNTER ? 12 :
                          enemy->kind == ENEMY_SNIPER ? 18 :
                          enemy->kind == ENEMY_BRUTE ? 20 : 26;
    }
    if (enemy->fire_timer > 0) {
      enemy->fire_timer--;
    } else if (enemy->kind == ENEMY_SNIPER) {
      create_shot(game, SHOT_ENEMY, enemy->x, enemy->y,
                  game->player_x, game->player_y, 0, 0);
      enemy->fire_timer = 78 - game->wave * 2;
    } else if (enemy->kind == ENEMY_CORE) {
      fire_core_salvo(game, enemy);
      enemy->fire_timer = 68;
    }
    if (enemy->active &&
        magnitude(enemy->x - game->player_x) +
        magnitude(enemy->y - game->player_y) <=
          (enemy->kind == ENEMY_CORE ? 2 : 1)) {
      damage_player(game);
    }
  }
}

/* Pull in nearby energy and add both score and EMP charge. */
static void collect_drops(struct Game* game) {
  int reach = 1 + game->magnet_level * 2;
  for (int i = 0; i < MAX_DROPS; ++i) {
    struct Drop* drop = &game->drops[i];
    if (drop->active &&
        magnitude(drop->x - game->player_x) +
        magnitude(drop->y - game->player_y) <= reach) {
      drop->active = 0;
      game->score += 10;
      if (game->score > game->best) {
        game->best = game->score;
      }
      game->pulse_charge += 45;
      if (game->pulse_charge > PULSE_FULL) {
        game->pulse_charge = PULSE_FULL;
      }
      game->sound_events |= SOUND_PICKUP;
    }
  }
}

/* Exclude choices whose capped effect would do nothing in this run. */
static int upgrade_available(struct Game* game, int kind) {
  if (kind == UPGRADE_RAPID) return game->rapid_level < 3;
  if (kind == UPGRADE_TWIN) return game->twin_level < 2;
  if (kind == UPGRADE_EMP) return game->emp_level < 3;
  if (kind == UPGRADE_MAGNET) return game->magnet_level < 3;
  if (kind == UPGRADE_REPAIR) return game->hp < game->max_hp;
  return 1;
}

/* Offer three different useful upgrades after each cleared nonfinal wave. */
static void choose_upgrade_options(struct Game* game) {
  for (int option = 0; option < 3; ++option) {
    int start = (int)(next_random(game) % UPGRADE_COUNT);
    int chosen = UPGRADE_HULL;
    for (int attempt = 0; attempt < UPGRADE_COUNT; ++attempt) {
      int candidate = (start + attempt) % UPGRADE_COUNT;
      int unique = upgrade_available(game, candidate);
      for (int prior = 0; prior < option; ++prior) {
        if (game->upgrade_choices[prior] == candidate) {
          unique = 0;
        }
      }
      if (unique) {
        chosen = candidate;
        break;
      }
    }
    game->upgrade_choices[option] = chosen;
  }
}

/* Apply the selected permanent upgrade and begin the next wave. */
static void apply_upgrade(struct Game* game, int choice) {
  int kind = game->upgrade_choices[choice];
  if (kind == UPGRADE_RAPID) {
    if (game->rapid_level < 3) game->rapid_level++;
  } else if (kind == UPGRADE_TWIN) {
    if (game->twin_level < 2) game->twin_level++;
  } else if (kind == UPGRADE_HULL) {
    game->hull_level++;
    game->max_hp += 2;
    game->hp += 2;
  } else if (kind == UPGRADE_EMP) {
    if (game->emp_level < 3) game->emp_level++;
  } else if (kind == UPGRADE_MAGNET) {
    if (game->magnet_level < 3) game->magnet_level++;
  } else if (kind == UPGRADE_REPAIR) {
    game->hp = game->max_hp;
  }
  game->wave++;
  begin_wave(game);
}

/* Report concise labels in the three-choice intermission panel. */
char* game_upgrade_name(int kind) {
  if (kind == UPGRADE_RAPID) return "OVERCLOCK";
  if (kind == UPGRADE_TWIN) return "TRIPLE SHOT";
  if (kind == UPGRADE_HULL) return "HULL PLATING";
  if (kind == UPGRADE_EMP) return "NOVA COIL";
  if (kind == UPGRADE_MAGNET) return "TRACTOR FIELD";
  return "FULL REPAIR";
}

/* Describe the game effect without requiring the player to know code values. */
char* game_upgrade_effect(int kind) {
  if (kind == UPGRADE_RAPID) return "Faster auto fire";
  if (kind == UPGRADE_TWIN) return "Extra wing shots";
  if (kind == UPGRADE_HULL) return "+2 max hull";
  if (kind == UPGRADE_EMP) return "Stronger nova";
  if (kind == UPGRADE_MAGNET) return "Pull distant energy";
  return "Restore all hull";
}

/* Advance the complete state machine by one frame. */
void game_tick(struct Game* game, struct GameInput input) {
  game->frame++;
  game->sound_events = 0;
  if (game->phase == PHASE_TITLE) {
    if (input.start) reset_run(game);
    return;
  }
  if (game->phase == PHASE_DEAD || game->phase == PHASE_VICTORY) {
    if (input.start) reset_run(game);
    return;
  }
  if (game->phase == PHASE_UPGRADE) {
    if (input.choose >= 1 && input.choose <= 3) {
      apply_upgrade(game, input.choose - 1);
    }
    return;
  }

  game->wave_frame++;
  if (game->hurt_timer > 0) game->hurt_timer--;
  if (game->pulse_flash > 0) game->pulse_flash--;
  if (game->pulse_charge < PULSE_FULL) {
    game->pulse_charge += 1 + game->emp_level;
    if (game->pulse_charge > PULSE_FULL) game->pulse_charge = PULSE_FULL;
  }
  move_player(game, input);
  if (input.pulse) trigger_pulse(game);

  if (game->wave_frame % 40 == 0 && game->spawned < game->spawn_budget) {
    int kind;
    if (game->wave == FINAL_WAVE && game->spawned == 0) {
      kind = ENEMY_CORE;
    } else {
      unsigned roll = next_random(game) % 10;
      kind = roll < 5 ? ENEMY_HUNTER :
             roll < 8 && game->wave >= 2 ? ENEMY_SNIPER :
             game->wave >= 3 ? ENEMY_BRUTE : ENEMY_HUNTER;
    }
    if (spawn_enemy(game, kind)) game->spawned++;
  }

  fire_player_weapon(game);
  move_shots(game);
  move_enemies(game);
  collect_drops(game);
  if (game->phase != PHASE_PLAY) return;

  if (game->spawned == game->spawn_budget && game_enemy_count(game) == 0) {
    if (game->wave == FINAL_WAVE) {
      game->phase = PHASE_VICTORY;
    } else {
      if (game->hp < game->max_hp) game->hp++;
      choose_upgrade_options(game);
      game->phase = PHASE_UPGRADE;
    }
    game->sound_events |= SOUND_WAVE;
  }
}
