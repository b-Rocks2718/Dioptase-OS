#ifndef STARFALL_H
#define STARFALL_H

#include "../crt/stdbool.h"

/* The arena leaves room in the 80x60 tile display for a status panel. */
#define FIELD_W 52
#define FIELD_H 38
/* Kept literal because bcc requires a literal bound in struct array members. */
#define FIELD_CELLS 1976
#define MAX_ENEMIES 56
#define MAX_SHOTS 112
#define MAX_DROPS 40
#define FINAL_WAVE 5
#define PULSE_FULL 180

/* The program owns all of these objects while it runs in user mode. */
enum GamePhase {
  PHASE_TITLE,
  PHASE_PLAY,
  PHASE_UPGRADE,
  PHASE_DEAD,
  PHASE_VICTORY
};

/* A cell is either open space or a solid asteroid. */
enum CellKind {
  CELL_SPACE,
  CELL_ROCK
};

/* Enemy kinds have distinct movement, health, and firing patterns. */
enum EnemyKind {
  ENEMY_HUNTER,
  ENEMY_SNIPER,
  ENEMY_BRUTE,
  ENEMY_CORE
};

/* Shots from opposing sides share one bounded pool. */
enum ShotSide {
  SHOT_PLAYER,
  SHOT_ENEMY
};

/* Upgrade choices persist through all five waves of a run. */
enum UpgradeKind {
  UPGRADE_RAPID,
  UPGRADE_TWIN,
  UPGRADE_HULL,
  UPGRADE_EMP,
  UPGRADE_MAGNET,
  UPGRADE_REPAIR,
  UPGRADE_COUNT
};

/* Sound bits let the synthesizer react once to several events in one frame. */
enum SoundEvent {
  SOUND_FIRE = 1,
  SOUND_DAMAGE = 2,
  SOUND_KILL = 4,
  SOUND_PULSE = 8,
  SOUND_PICKUP = 16,
  SOUND_WAVE = 32
};

/* One enemy occupies a center cell; the core is drawn as a 3x3 figure. */
struct Enemy {
  int active;
  int kind;
  int x;
  int y;
  int hp;
  int move_timer;
  int fire_timer;
};

/* Positions and velocities use 8 fractional bits per arena cell. */
struct Shot {
  int active;
  int side;
  int fx;
  int fy;
  int vx;
  int vy;
  int damage;
  int pierce;
  int ttl;
};

/* Energy drops restore EMP charge when collected. */
struct Drop {
  int active;
  int x;
  int y;
};

/* Each frame carries pressed actions; movement is extended briefly by the game. */
struct GameInput {
  int move_x;
  int move_y;
  int move_pressed;
  int pulse;
  int start;
  int choose;
};

/* This is the entire deterministic state of one arcade run. */
struct Game {
  unsigned rng;
  unsigned frame;
  unsigned score;
  unsigned best;
  int phase;
  int wave;
  int wave_frame;
  int spawn_budget;
  int spawned;
  int kills;
  int player_x;
  int player_y;
  int hp;
  int max_hp;
  int hurt_timer;
  int move_timer;
  int move_x;
  int move_y;
  int move_latch;
  int fire_timer;
  int pulse_charge;
  int pulse_flash;
  int rapid_level;
  int twin_level;
  int hull_level;
  int emp_level;
  int magnet_level;
  int upgrade_choices[3];
  int sound_events;
  unsigned char cells[FIELD_CELLS];
  unsigned char stars[FIELD_CELLS];
  struct Enemy enemies[MAX_ENEMIES];
  struct Shot shots[MAX_SHOTS];
  struct Drop drops[MAX_DROPS];
};

/* Initialize the title screen without starting a run. */
void game_init(struct Game* game, unsigned seed, unsigned best);

/* Advance one 60 Hz frame using the actions pressed since the previous frame. */
void game_tick(struct Game* game, struct GameInput input);

/* Return the number of active enemies, including the final core. */
int game_enemy_count(struct Game* game);

/* Return the user-facing label for an upgrade choice. */
char* game_upgrade_name(int kind);

/* Return a short effect description for an upgrade choice. */
char* game_upgrade_effect(int kind);

#endif
