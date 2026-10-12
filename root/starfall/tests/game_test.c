/*
 * Deterministic host checks for Starfall's user-mode game rules. These test
 * transitions, combat, and bounds without depending on VGA timing or input.
 */

#include "../starfall.h"

#include <assert.h>
#include <stdio.h>

static struct Game game;

/* Feed a blank action frame to advance scripted situations. */
static struct GameInput blank_input(void) {
  struct GameInput input = {0};
  return input;
}

/* Start with a known seed so procedural placement is reproducible. */
static void launch(void) {
  game_init(&game, 0xA5A5A5A5u, 70);
  assert(game.phase == PHASE_TITLE);
  struct GameInput input = blank_input();
  input.start = 1;
  game_tick(&game, input);
  assert(game.phase == PHASE_PLAY);
  assert(game.wave == 1);
  assert(game.hp == 6);
  assert(game.best == 70);
}

/* Arena generation must always protect the center and enclose the playfield. */
static void test_field(void) {
  launch();
  assert(FIELD_CELLS == FIELD_W * FIELD_H);
  assert(game.cells[game.player_y * FIELD_W + game.player_x] == CELL_SPACE);
  for (int x = 0; x < FIELD_W; ++x) {
    assert(game.cells[x] == CELL_ROCK);
    assert(game.cells[(FIELD_H - 1) * FIELD_W + x] == CELL_ROCK);
  }
  for (int y = 0; y < FIELD_H; ++y) {
    assert(game.cells[y * FIELD_W] == CELL_ROCK);
    assert(game.cells[y * FIELD_W + FIELD_W - 1] == CELL_ROCK);
  }
}

/* A key press moves the ship and expires after its short repeat latch. */
static void test_movement(void) {
  launch();
  int before = game.player_x;
  struct GameInput input = blank_input();
  input.move_pressed = 1;
  input.move_x = 1;
  game_tick(&game, input);
  assert(game.player_x == before + 1);
  for (int i = 0; i < 18; ++i) game_tick(&game, blank_input());
  int stopped = game.player_x;
  for (int i = 0; i < 18; ++i) game_tick(&game, blank_input());
  assert(game.player_x == stopped);
}

/* Nova consumes charge, kills nearby enemies, and erases hostile shots. */
static void test_nova(void) {
  launch();
  game.enemies[0].active = 1;
  game.enemies[0].kind = ENEMY_HUNTER;
  game.enemies[0].x = game.player_x + 2;
  game.enemies[0].y = game.player_y;
  game.enemies[0].hp = 2;
  game.enemies[0].move_timer = 100;
  game.shots[0].active = 1;
  game.shots[0].side = SHOT_ENEMY;
  game.shots[0].fx = (game.player_x + 2) * 256 + 128;
  game.shots[0].fy = game.player_y * 256 + 128;
  game.shots[0].ttl = 10;
  struct GameInput input = blank_input();
  input.pulse = 1;
  game_tick(&game, input);
  assert(game.pulse_charge == 0);
  assert(!game.enemies[0].active);
  assert(!game.shots[0].active);
  assert(game.score >= 20);
  assert(game.sound_events & SOUND_PULSE);
}

/* A successful auto-target volley should kill a stationary nearby hunter. */
static void test_weapon(void) {
  launch();
  game.enemies[0].active = 1;
  game.enemies[0].kind = ENEMY_HUNTER;
  game.enemies[0].x = game.player_x + 4;
  game.enemies[0].y = game.player_y;
  game.enemies[0].hp = 2;
  game.enemies[0].move_timer = 1000;
  game.enemies[0].fire_timer = 1000;
  for (int i = 0; i < 28; ++i) game_tick(&game, blank_input());
  assert(!game.enemies[0].active);
  assert(game.kills == 1);
}

/* Invulnerability prevents two same-frame hits from draining several hull. */
static void test_hurt_window(void) {
  launch();
  for (int i = 0; i < 2; ++i) {
    game.shots[i].active = 1;
    game.shots[i].side = SHOT_ENEMY;
    game.shots[i].fx = game.player_x * 256 + 128;
    game.shots[i].fy = game.player_y * 256 + 128;
    game.shots[i].ttl = 2;
  }
  game_tick(&game, blank_input());
  assert(game.hp == 5);
  assert(game.hurt_timer > 0);
}

/* Each clear gives three distinct upgrades; the fifth clear wins the run. */
static void test_progression(void) {
  launch();
  for (int wave = 1; wave <= FINAL_WAVE; ++wave) {
    game.spawn_budget = 0;
    game.spawned = 0;
    game_tick(&game, blank_input());
    if (wave == FINAL_WAVE) {
      assert(game.phase == PHASE_VICTORY);
      break;
    }
    assert(game.phase == PHASE_UPGRADE);
    assert(game.upgrade_choices[0] != game.upgrade_choices[1]);
    assert(game.upgrade_choices[1] != game.upgrade_choices[2]);
    assert(game.upgrade_choices[0] != game.upgrade_choices[2]);
    struct GameInput input = blank_input();
    input.choose = 1;
    game_tick(&game, input);
    assert(game.phase == PHASE_PLAY);
    assert(game.wave == wave + 1);
  }
}

/* Mixed scripted input exercises bounded pools and procedural waves for many frames. */
static void test_long_run(void) {
  launch();
  for (int frame = 0; frame < 3000; ++frame) {
    struct GameInput input = blank_input();
    if (frame % 7 == 0) {
      input.move_pressed = 1;
      input.move_x = frame % 28 < 14 ? 1 : -1;
    }
    if (frame % 160 == 0) input.pulse = 1;
    if (game.phase == PHASE_UPGRADE) input.choose = 1;
    if (game.phase == PHASE_DEAD) input.start = 1;
    game_tick(&game, input);
    assert(game.player_x >= 1 && game.player_x < FIELD_W - 1);
    assert(game.player_y >= 1 && game.player_y < FIELD_H - 1);
    for (int i = 0; i < MAX_ENEMIES; ++i) {
      if (game.enemies[i].active) {
        assert(game.enemies[i].x >= 1 && game.enemies[i].x < FIELD_W - 1);
        assert(game.enemies[i].y >= 1 && game.enemies[i].y < FIELD_H - 1);
      }
    }
  }
}

/* Rank a nearby destination by walls, edge exposure, enemies, and hostile bolts. */
static int tactical_score(int x, int y) {
  if (x < 2 || y < 2 || x >= FIELD_W - 2 || y >= FIELD_H - 2 ||
      game.cells[y * FIELD_W + x] == CELL_ROCK) return -100000;
  int dx = x - FIELD_W / 2;
  int dy = y - FIELD_H / 2;
  int score = -(dx < 0 ? -dx : dx) - (dy < 0 ? -dy : dy);
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (!game.enemies[i].active) continue;
    dx = x - game.enemies[i].x;
    dy = y - game.enemies[i].y;
    int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (distance < 8) score -= (8 - distance) * (8 - distance) * 5;
  }
  for (int i = 0; i < MAX_SHOTS; ++i) {
    if (!game.shots[i].active || game.shots[i].side != SHOT_ENEMY) continue;
    dx = x - (game.shots[i].fx >> 8);
    dy = y - (game.shots[i].fy >> 8);
    int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (distance < 5) score -= (5 - distance) * (5 - distance) * 5;
  }
  return score;
}

/* Complete a real seeded five-wave run using only public frame actions. */
static void test_playable_run(void) {
  game_init(&game, 2, 0);
  struct GameInput input = blank_input();
  input.start = 1;
  game_tick(&game, input);
  for (int frame = 0; frame < 14000 &&
                      game.phase != PHASE_DEAD &&
                      game.phase != PHASE_VICTORY; ++frame) {
    input = blank_input();
    if (game.phase == PHASE_UPGRADE) {
      int choice = 0;
      for (int i = 0; i < 3; ++i) {
        int kind = game.upgrade_choices[i];
        if (kind == UPGRADE_REPAIR && game.hp < game.max_hp / 2) {
          choice = i;
          break;
        }
        if (kind == UPGRADE_RAPID) choice = i;
        else if (kind == UPGRADE_TWIN &&
                 game.upgrade_choices[choice] != UPGRADE_RAPID) choice = i;
        else if (kind == UPGRADE_HULL &&
                 game.upgrade_choices[choice] != UPGRADE_RAPID &&
                 game.upgrade_choices[choice] != UPGRADE_TWIN) choice = i;
      }
      input.choose = choice + 1;
    } else {
      int nearest = 10000;
      for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (!game.enemies[i].active) continue;
        int dx = game.enemies[i].x - game.player_x;
        int dy = game.enemies[i].y - game.player_y;
        int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (distance < nearest) nearest = distance;
      }
      if (frame % 3 == 0) {
        int directions_x[4] = {1, -1, 0, 0};
        int directions_y[4] = {0, 0, 1, -1};
        int best_score = -1000000;
        int best_direction = 0;
        for (int direction = 0; direction < 4; ++direction) {
          int score = tactical_score(game.player_x + directions_x[direction] * 3,
                                     game.player_y + directions_y[direction] * 3);
          if ((frame / 160 + direction) % 4 == 0) score += 4;
          if (score > best_score) {
            best_score = score;
            best_direction = direction;
          }
        }
        input.move_pressed = 1;
        input.move_x = directions_x[best_direction];
        input.move_y = directions_y[best_direction];
      }
      if (nearest < 7 && game.pulse_charge >= 180) input.pulse = 1;
    }
    game_tick(&game, input);
  }
  assert(game.phase == PHASE_VICTORY);
  assert(game.kills > 60);
  assert(game.score > 1000);
}

/* Run the independent checks as one lightweight host test command. */
int main(void) {
  test_field();
  test_movement();
  test_nova();
  test_weapon();
  test_hurt_window();
  test_progression();
  test_long_run();
  test_playable_run();
  puts("starfall game tests: passed");
  return 0;
}
