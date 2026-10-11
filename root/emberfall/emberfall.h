/* Emberfall: fantasy horde survival for Dioptase.
 *
 * Pure game rules with no syscalls, no display, no audio, and no heap use.
 * Positions use 8.8 fixed point (256 units per pixel) so all movement stays
 * in 32-bit integer math. Distances use squared pixel checks to avoid sqrt.
 * The host unit test compiles game.c directly with gcc; bcc builds the same
 * file for the guest. Struct array bounds stay literal for bcc.
 */

#ifndef EMBERFALL_H
#define EMBERFALL_H

#include "../crt/stdbool.h"

/* Pixel playfield is the mapped prefix of the 320x240 buffer (rows 0..235). */
#define PIXEL_W 320
#define PIXEL_H 236
#define FIXED_SHIFT 8
#define FIXED_ONE 256

/* Transparent art marker as signed short so short comparisons match.
 * Plain 0xF000 is int 61440 while (short)0xF000 is -4096; an uncast
 * define never equals a short pixel and leaks black boxes on screen. */
#define TRANSPARENT_PX ((short)0xF000)

/* Bounded pools live inside struct Game so no malloc is needed. */
#define MAX_ENEMIES 48
#define MAX_PROJECTILES 64
#define MAX_GEMS 64
#define MAX_PARTICLES 96

/* Spells, passives, and level-up choices. */
#define SPELL_COUNT 5
#define PASSIVE_COUNT 5
#define MAX_SPELL_LEVEL 5
#define MAX_PASSIVE_LEVEL 5
#define UPGRADE_CHOICES 3

/* Screens the game can show. HELP returns to return_phase. */
enum GamePhase {
  PHASE_TITLE,
  PHASE_PLAY,
  PHASE_LEVELUP,
  PHASE_PAUSE,
  PHASE_GAMEOVER,
  PHASE_HELP
};

/* Enemy kinds in unlock order. */
enum EnemyKind {
  ENEMY_SLIME,
  ENEMY_BAT,
  ENEMY_SKELETON,
  ENEMY_ORC,
  ENEMY_WRAITH,
  ENEMY_GOLEM,
  ENEMY_BOSS,
  ENEMY_COUNT
};

/* Spells the warden can learn. */
enum SpellKind {
  SPELL_EMBER,
  SPELL_FIREBALL,
  SPELL_FROST,
  SPELL_ARC,
  SPELL_NOVA
};

/* Passives that boost the warden. */
enum PassiveKind {
  PASSIVE_SWIFT,
  PASSIVE_POWER,
  PASSIVE_VITAL,
  PASSIVE_MAGNET,
  PASSIVE_TOME
};

/* Level-up kinds: spells, passives, then repeatable bonuses. */
enum UpgradeKind {
  UPGRADE_SPELL_EMBER,
  UPGRADE_SPELL_FIREBALL,
  UPGRADE_SPELL_FROST,
  UPGRADE_SPELL_ARC,
  UPGRADE_SPELL_NOVA,
  UPGRADE_PASSIVE_SWIFT,
  UPGRADE_PASSIVE_POWER,
  UPGRADE_PASSIVE_VITAL,
  UPGRADE_PASSIVE_MAGNET,
  UPGRADE_PASSIVE_TOME,
  UPGRADE_BONUS_HEAL,
  UPGRADE_BONUS_SCORE,
  UPGRADE_COUNT
};

/* One-shot sound requests for the audio mixer, one bit per event. */
enum SoundEvent {
  SOUND_SHOOT = 1,
  SOUND_HIT = 2,
  SOUND_EXPLODE = 4,
  SOUND_PICKUP = 8,
  SOUND_LEVELUP = 16,
  SOUND_HURT = 32,
  SOUND_BOSS = 64,
  SOUND_GAMEOVER = 128,
  SOUND_UI = 256
};

/* Warden state. Cooldowns count down to zero, then the spell fires. */
struct Player {
  int x;
  int y;
  int hp;
  int max_hp;
  int level;
  int xp;
  int xp_next;
  int invuln_timer;
  int regen_timer;
  int spell_level[5];
  int passive_level[5];
  int cooldown[5];
  /* Last nonzero move direction, each -1/0/1; picks the player sprite. */
  int face_x;
  int face_y;
};

/* One live enemy seeking the warden. */
struct Enemy {
  int active;
  int kind;
  int x;
  int y;
  int hp;
  int max_hp;
  int speed;
  int damage;
  int slow_timer;
  int hit_timer;
};

/* One flying bolt, fireball, or frost shard. */
struct Projectile {
  int active;
  int kind;
  int x;
  int y;
  int vx;
  int vy;
  int damage;
  int pierce;
  int life;
};

/* One XP gem with scatter velocity and magnet pull. */
struct Gem {
  int active;
  int x;
  int y;
  int vx;
  int vy;
  int value;
};

/* One short-lived spark, puff, or ring fragment. */
struct Particle {
  int active;
  int x;
  int y;
  int vx;
  int vy;
  int life;
  int max_life;
  int color;
};

/* One level-up card. level is the resulting spell/passive level, or 0. */
struct UpgradeChoice {
  int kind;
  int level;
};

/* Abstracted input from keyboard and mouse. choose is -1 or 0..2. */
struct GameInput {
  int move_x;
  int move_y;
  int start;
  int pause;
  int choose;
  int confirm;
};

/* Complete run state. Large pools stay here, outside the 16 KiB stack. */
struct Game {
  int phase;
  int return_phase;
  int tick;
  unsigned rng;
  struct Player player;
  struct Enemy enemies[48];
  struct Projectile projectiles[64];
  struct Gem gems[64];
  struct Particle particles[96];
  struct UpgradeChoice choices[3];
  int spawn_timer;
  int boss_timer;
  int boss_count;
  int kills;
  int score;
  int best;
  int sound;
  int shake_timer;
  int streak;
  int last_kill_tick;
};

/* Start a run with a seed and the saved best score. */
void game_init(struct Game* game, unsigned seed, unsigned best);

/* Advance one 30 Hz tick using abstracted input. */
void game_tick(struct Game* game, struct GameInput input);

/* Titles and one-line descriptions for level-up cards. */
char* upgrade_title(int kind);
char* upgrade_desc(int kind, int level);

/* Seconds survived, for HUD and high scores. */
int game_seconds(struct Game* game);

#endif
