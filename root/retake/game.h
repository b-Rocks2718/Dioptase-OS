#ifndef RETAKE_GAME_H
#define RETAKE_GAME_H

#include "player.h"
#include "takes.h"

/*
 * Game rules above the movement core: rooms and transitions, health,
 * hazards and respawn, persistent progress, entities and combat, saving.
 * Written by Opus while Astra was away; listed in COLLAB.md's review queue.
 *
 * Tick order: takes advance -> platforms -> player_step -> immediate hazard
 * resolution -> capture -> jump cut -> entities -> switches/gates -> combat
 * and death -> pickups/interactions -> room transitions. Death/hazards cannot
 * be bypassed by a same-tick door or healing pickup. Inputs are deterministic.
 */

/* Entity kinds; tools/entities.py mirrors these codes for the room files. */
#define ENT_NONE 0
#define ENT_COIN 1       /* a: value (default 1); persistent */
#define ENT_HEART 2      /* restores one frame of health */
#define ENT_FRAME 3      /* +1 max health; persistent */
#define ENT_REEL 4       /* a: lore script; persistent */
#define ENT_ABILITY 5    /* a: AB_* bit, b: script shown; persistent */
#define ENT_MIRROR 6     /* save point (interact) */
#define ENT_SIGN 7       /* a: script (interact) */
#define ENT_NPC 8        /* a: script, b: look (interact) */
#define ENT_PLATE 9      /* a: switch id, pressed by Ruby or any take */
#define ENT_GATE 10      /* a: switch id, b: height in tiles; solid unless its switch is pressed */
#define ENT_DOOR 11      /* a: target room, b: target tx, c: target ty (interact) */
#define ENT_WALKER 12    /* patrols, turns at walls and ledges */
#define ENT_HOPPER 13    /* hops toward Ruby */
#define ENT_FLYER 14     /* drifts in a wave, swoops when close */
#define ENT_THROWER 15   /* throws arcing projectiles */
#define ENT_CHARGER 16   /* rushes along the floor when Ruby is level with it */
#define ENT_SHOT 17      /* projectile (spawned) */
#define ENT_SANDBAG 18   /* drops when Ruby passes beneath, then rests as a block */
#define ENT_SPRING 19    /* bounces Ruby high */
#define ENT_BOSS 20      /* a: boss id */
#define ENT_SHOP 21      /* the Prop Department counter */
#define ENT_WAVE 22      /* a boss's ground shockwave (spawned) */
#define ENT_PLATFORM 23  /* moving platform: a range tiles, b axis (0 x, 1 y), c speed Q8/tick, d width tiles */
#define ENT_TENTACLE 24  /* the Kraken's tentacle (spawned by boss 3) */
#define ENT_KINDS 25

#define MAX_ENTS 40
#define MAX_FLAGS 2048
#define MAX_SWITCHES 16

/* One room-local actor or prop; dead reusable slots have kind ENT_NONE. */
struct Ent {
  int kind;
  int x;           /* Q8 top-left of the hitbox */
  int y;
  int vx;
  int vy;
  int w;           /* Q8 hitbox size */
  int h;
  int hp;
  int state;
  int timer;
  int dir;
  int a;
  int b;
  int c;
  int d;
  int flag;        /* persistent flag id, or -1 */
  int alive;
  int hurt;        /* invulnerability ticks after a hit */
  int anim;
  int tx;          /* spawn tile, for gates and resets */
  int ty;
};

/* Persistent progress (saved by mirrors). */
struct Progress {
  int max_health;
  int scrip;
  int abilities;
  int take_slots;
  int take_frames;
  int save_room;
  int save_x;
  int save_y;
  unsigned play_ticks;
  unsigned flags[64];      /* MAX_FLAGS bits */
  unsigned visited[8];     /* rooms seen, one bit per room */
};

/* Presentation events raised by game_tick for audio and effects. */
#define GE_COIN 1
#define GE_PICKUP 2
#define GE_HIT 4         /* Ruby's (or a take's) clap connected */
#define GE_KILL 8
#define GE_HURT 16
#define GE_HAZARD 32
#define GE_DEATH 64
#define GE_ROOM 128      /* a new room was loaded this tick */
#define GE_SAVE 256
#define GE_JUMPCUT 512
#define GE_SPRING 1024
#define GE_GATE 2048
#define GE_REC_START 4096
#define GE_REC_STOP 8192
#define GE_BOSS_HIT 16384
#define GE_BOSS_DOWN 32768
#define GE_SLAM 65536

extern struct Player g_hero;
extern struct Takes g_takes;
extern struct Progress g_prog;
extern struct Ent g_ents[40];
extern int g_ent_count;
extern int g_health;
extern unsigned g_events;
extern int g_pending_script;   /* script to show after this tick, or -1 */
extern int g_pending_shop;     /* the Prop Department counter was used this tick */
extern int g_switch[16];
extern struct Ent* g_boss;     /* the room's boss, or 0 */

void game_new(int start_room);
int game_load(void);
int game_save(void);
void game_enter_room(int room, int x_px, int y_px);
void game_tick(int held, int pressed);
int flag_get(int id);
#define BOSS_FLAG_BASE 2000      /* flag ids 2000+ mark defeated bosses */
void flag_set(int id);

#endif
