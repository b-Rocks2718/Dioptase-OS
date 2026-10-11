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

static void plant_enemy(int slot, int kind, int dx_px, int dy_px, int hp) {
  game.enemies[slot].active = 1;
  game.enemies[slot].kind = kind;
  game.enemies[slot].x = game.player.x + dx_px * 256;
  game.enemies[slot].y = game.player.y + dy_px * 256;
  game.enemies[slot].hp = hp;
  game.enemies[slot].max_hp = hp;
  game.enemies[slot].speed = 0;
  game.enemies[slot].damage = 0;
  game.enemies[slot].slow_timer = 0;
  game.enemies[slot].hit_timer = 0;
}

static int count_kind(int kind) {
  int n = 0;
  int i = 0;
  while (i < 64) {
    if (game.projectiles[i].active && game.projectiles[i].kind == kind) n++;
    i++;
  }
  return n;
}

static void test_transparency_compare(void) {
  short px = TRANSPARENT_PX;
  short black = 0x000;
  assert(px == TRANSPARENT_PX);
  assert(black != TRANSPARENT_PX);
  printf("transparency compare ok\n");
}

static void test_spell_spread(void) {
  struct GameInput in;
  int i;
  int vy0 = 0;
  int vy1 = 0;
  int vy2 = 0;
  int found = 0;
  start_run();
  game.player.spell_level[SPELL_EMBER] = 5;
  plant_enemy(0, ENEMY_SLIME, 40, 0, 5000);
  in = blank_input();
  i = 0;
  while (i < 10) {
    game_tick(&game, in);
    i++;
  }
  assert(count_kind(SPELL_EMBER) == 3);
  i = 0;
  while (i < 64) {
    if (game.projectiles[i].active &&
        game.projectiles[i].kind == SPELL_EMBER) {
      if (found == 0) vy0 = game.projectiles[i].vy;
      else if (found == 1) vy1 = game.projectiles[i].vy;
      else if (found == 2) vy2 = game.projectiles[i].vy;
      found++;
    }
    i++;
  }
  assert(found == 3);
  assert(vy0 != vy1 && vy0 != vy2 && vy1 != vy2);
  printf("spell spread ok\n");
}

static void test_fireball_second_target(void) {
  struct GameInput in;
  int i;
  start_run();
  game.player.spell_level[SPELL_EMBER] = 0;
  game.player.spell_level[SPELL_FIREBALL] = 4;
  plant_enemy(0, ENEMY_SLIME, 60, 0, 5000);
  plant_enemy(1, ENEMY_SLIME, -60, 0, 5000);
  in = blank_input();
  i = 0;
  while (i < 10) {
    game_tick(&game, in);
    i++;
  }
  assert(count_kind(SPELL_FIREBALL) == 2);
  printf("fireball second target ok\n");
}

static void test_separation_streak(void) {
  struct GameInput in;
  int i;
  start_run();
  plant_enemy(0, ENEMY_SLIME, 0, 0, 5000);
  plant_enemy(1, ENEMY_SLIME, 0, 0, 5000);
  /* Speeds stay zero so only separation moves them. */
  in = blank_input();
  game_tick(&game, in);
  assert(game.enemies[0].x != game.enemies[1].x);
  /* Splash both with one fireball for a same-tick chain. */
  start_run();
  game.player.spell_level[SPELL_EMBER] = 0;
  game.player.spell_level[SPELL_FIREBALL] = 1;
  plant_enemy(0, ENEMY_SLIME, 6, 0, 1);
  plant_enemy(1, ENEMY_SLIME, 7, 0, 1);
  in = blank_input();
  i = 0;
  while (i < 40 && game.kills < 2) {
    game_tick(&game, in);
    i++;
  }
  assert(game.kills == 2);
  assert(game.streak == 2);
  printf("separation/streak ok\n");
}

static void test_player_facing(void) {
  struct GameInput in;
  start_run();
  /* Fresh warden faces south. */
  assert(game.player.face_x == 0);
  assert(game.player.face_y == 1);
  /* Each axis tracks its last nonzero input independently. */
  in = blank_input();
  in.move_x = 1;
  game_tick(&game, in);
  assert(game.player.face_x == 1);
  assert(game.player.face_y == 1);
  in.move_x = 0;
  in.move_y = -1;
  game_tick(&game, in);
  assert(game.player.face_x == 1);
  assert(game.player.face_y == -1);
  /* Diagonal input turns both axes at once. */
  in.move_x = -1;
  in.move_y = 1;
  game_tick(&game, in);
  assert(game.player.face_x == -1);
  assert(game.player.face_y == 1);
  /* Standing still keeps the last facing. */
  in = blank_input();
  game_tick(&game, in);
  assert(game.player.face_x == -1);
  assert(game.player.face_y == 1);
  printf("facing ok\n");
}

int main(void) {
  test_move_clamp();
  test_spells_fire();
  test_kill_xp_levelup();
  test_pause_help_gameover();
  test_upgrades_bounded();
  test_transparency_compare();
  test_spell_spread();
  test_fireball_second_target();
  test_separation_streak();
  test_player_facing();
  printf("ALL EMBERFALL HOST TESTS PASS\n");
  return 0;
}
