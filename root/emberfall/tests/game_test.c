/* Host checks for Emberfall rules: start, move, combat, XP, upgrades. */

#include "../emberfall.h"

#include <assert.h>
#include <stdio.h>

static struct Game game;

static struct GameInput blank_input(void) {
  struct GameInput in;
  in.move_x = 0;
  in.move_y = 0;
  in.start = 0;
  in.pause = 0;
  in.choose = -1;
  in.confirm = 0;
  return in;
}

static void start_run(void) {
  struct GameInput in;
  game_init(&game, 0x1234u, 70);
  assert(game.phase == PHASE_TITLE);
  assert(game.best == 70);
  in = blank_input();
  in.start = 1;
  game_tick(&game, in);
  assert(game.phase == PHASE_PLAY);
  assert(game.player.hp == 100);
  assert(game.player.spell_level[SPELL_EMBER] == 1);
}

static void test_move_clamp(void) {
  struct GameInput in;
  int i;
  start_run();
  in = blank_input();
  in.move_x = -1;
  in.move_y = -1;
  i = 0;
  while (i < 400) {
    game_tick(&game, in);
    assert(game.phase == PHASE_PLAY || game.phase == PHASE_LEVELUP ||
      game.phase == PHASE_PAUSE);
    if (game.phase != PHASE_PLAY) break;
    i++;
  }
  assert((game.player.x >> 8) >= 10);
  assert((game.player.y >> 8) >= 10);
  printf("move/clamp ok\n");
}

static void test_spells_fire(void) {
  struct GameInput in;
  int i;
  int fired;
  start_run();
  /* Plant a target next to the warden so bolts have somewhere to go. */
  game.enemies[0].active = 1;
  game.enemies[0].kind = ENEMY_SLIME;
  game.enemies[0].x = game.player.x + 20 * 256;
  game.enemies[0].y = game.player.y;
  game.enemies[0].hp = 5000;
  game.enemies[0].max_hp = 5000;
  game.enemies[0].speed = 0;
  game.enemies[0].damage = 0;
  game.enemies[0].slow_timer = 0;
  game.enemies[0].hit_timer = 0;
  in = blank_input();
  fired = 0;
  i = 0;
  while (i < 60) {
    game_tick(&game, in);
    if (game.sound & SOUND_SHOOT) fired = 1;
    if (game.phase != PHASE_PLAY) break;
    i++;
  }
  assert(fired);
  printf("spells fire ok\n");
}

static void test_kill_xp_levelup(void) {
  struct GameInput in;
  int i;
  start_run();
  game.enemies[1].active = 1;
  game.enemies[1].kind = ENEMY_SLIME;
  game.enemies[1].x = game.player.x + 6 * 256;
  game.enemies[1].y = game.player.y;
  game.enemies[1].hp = 1;
  game.enemies[1].max_hp = 1;
  game.enemies[1].speed = 0;
  game.enemies[1].damage = 0;
  game.enemies[1].slow_timer = 0;
  game.enemies[1].hit_timer = 0;
  /* Force many kills via direct damage path through projectiles. */
  game.player.xp = 0;
  in = blank_input();
  i = 0;
  while (i < 400 && game.phase == PHASE_PLAY) {
    /* Drop gems under the player to force XP gain deterministically. */
    if (i % 10 == 0) {
      int g = 0;
      while (g < 64) {
        if (!game.gems[g].active) {
          game.gems[g].active = 1;
          game.gems[g].x = game.player.x;
          game.gems[g].y = game.player.y;
          game.gems[g].vx = 0;
          game.gems[g].vy = 0;
          game.gems[g].value = 2;
          break;
        }
        g++;
      }
    }
    game_tick(&game, in);
    i++;
  }
  assert(game.phase == PHASE_LEVELUP);
  assert(game.player.xp >= game.player.xp_next);
  in = blank_input();
  in.choose = 0;
  {
    int before = game.player.level;
    game_tick(&game, in);
    assert(game.player.level == before + 1);
  }
  printf("kill/xp/levelup ok\n");
}

static void test_pause_help_gameover(void) {
  struct GameInput in;
  start_run();
  in = blank_input();
  in.pause = 1;
  game_tick(&game, in);
  assert(game.phase == PHASE_PAUSE);
  in = blank_input();
  in.pause = 1;
  game_tick(&game, in);
  assert(game.phase == PHASE_PLAY);
  /* Help from title returns to title. */
  game_init(&game, 99u, 0);
  in = blank_input();
  in.pause = 1;
  game_tick(&game, in);
  assert(game.phase == PHASE_HELP);
  in = blank_input();
  in.confirm = 1;
  game_tick(&game, in);
  assert(game.phase == PHASE_TITLE);
  /* Lethal hit ends the run and records best. */
  start_run();
  game.player.hp = 1;
  game.player.invuln_timer = 0;
  game.enemies[2].active = 1;
  game.enemies[2].kind = ENEMY_SLIME;
  game.enemies[2].x = game.player.x;
  game.enemies[2].y = game.player.y;
  game.enemies[2].hp = 50;
  game.enemies[2].max_hp = 50;
  game.enemies[2].speed = 0;
  game.enemies[2].damage = 50;
  game.enemies[2].slow_timer = 0;
  game.enemies[2].hit_timer = 0;
  game.score = 1234;
  in = blank_input();
  game_tick(&game, in);
  assert(game.phase == PHASE_GAMEOVER);
  assert(game.best == 1234);
  printf("pause/help/gameover ok\n");
}

static void test_upgrades_bounded(void) {
  int k;
  start_run();
  k = 0;
  while (k < 12) {
    char* t = upgrade_title(k);
    char* d = upgrade_desc(k, 1);
    assert(t != 0 && d != 0);
    assert(t[0] != 0 && d[0] != 0);
    k++;
  }
  printf("upgrade text ok\n");
}

int main(void) {
  test_move_clamp();
  test_spells_fire();
  test_kill_xp_levelup();
  test_pause_help_gameover();
  test_upgrades_bounded();
  printf("ALL EMBERFALL HOST TESTS PASS\n");
  return 0;
}
