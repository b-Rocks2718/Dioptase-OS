#ifndef RETAKE_PLAYER_H
#define RETAKE_PLAYER_H

/* Pure user-mode game simulation, shared by bcc and native verification.
 * Positions, velocities and collision rectangles use signed Q8 pixels.
 * Room data and platform arrays belong to the caller and remain valid for
 * the whole step. There is no MMIO, allocation, threading or syscall here.
 * ABI.md specifies 32-bit int; values stay within a 640x480-pixel room and
 * ordinary fixed-point arithmetic never depends on signed overflow. */

#define TILE 8
#define PLAYER_W 8
#define PLAYER_H 12
#define Q8 256

#define COL_AIR 0
#define COL_SOLID 1
#define COL_ONEWAY 2
#define COL_SPIKE 3

#define IN_LEFT 1
#define IN_RIGHT 2
#define IN_UP 4
#define IN_DOWN 8
#define IN_JUMP 16
#define IN_RECORD 32
#define IN_ATTACK 64
#define IN_JUMPCUT 128
#define IN_INTERACT 256
#define IN_REWIND 512
#define IN_OVERCRANK 1024

#define AB_JUMPCUT 1
#define AB_WIRE 2
#define AB_GLIDE 4
#define AB_REWIND 8
#define AB_OVERCRANK 16

#define EV_JUMP 1
#define EV_WALLJUMP 2
#define EV_LAND 4
#define EV_WIRE 8
#define EV_HAZARD 16
#define EV_CLAP 32
#define EV_GLIDE 64
#define EV_HURT 128

#define POSE_IDLE 0
#define POSE_RUN 1
#define POSE_RISE 2
#define POSE_FALL 3
#define POSE_WALL 4
#define POSE_GLIDE 5
#define POSE_CLAP 6
#define POSE_HURT 7

/* Axis-aligned Q8 rectangle; touching edges alone do not overlap. */
struct Hitbox {
  int x, y, w, h;
};

/* An immutable moving one-way surface for this tick. x/y are its new
 * position; dx/dy are displacement from the previous tick. id stays stable
 * while the surface exists. Disable active on a teleport/loop-reset tick.
 * Carry is clipped against live solid tiles; obstruction detaches a rider. */
struct Platform {
  int x, y, w, h;
  int dx, dy;
  int id;
  int active;
};

/* A room contains at most 80x60 8-pixel tiles. row[ty] must equal ty*w.
 * Outside the room and unknown tile values are solid. Spikes report contact
 * without blocking movement. One-way tiles block downward crossings only.
 * At most 16 platforms are supplied; they collide only with the live player.
 * Room transitions and mutable doors are managed by the game between ticks. */
struct Room {
  int w, h;
  unsigned char* col;
  int* row;
  struct Platform* platforms;
  int platform_count;
};

/* The live stunt double's motion and short-lived action state. x/y locate
 * the top-left of an 8x12 body. attack_dir is 0=none, 1=side, 2=up, 3=down;
 * attack_timer includes recovery: use player_attack_box for active frames.
 * riding is a stable platform id, or -1. The game owns health and progression.
 * Timers below riding are private simulation state, not rendering controls. */
struct Player {
  int x, y, vx, vy;
  int facing;
  int on_ground;
  int wall_dir;
  int abilities;
  int air_jump_left;
  int gliding;
  int attack_timer;
  int attack_dir;
  int hurt_timer;
  unsigned events;
  int pose;
  int riding;
  int coyote;
  int jump_buffer;
  int jump_hold;
  int wall_lock;
  int attack_cooldown;
  int drop_timer;
};

/* Start with zero motion, facing right, at a clear pixel-aligned location. */
void player_init(struct Player* p, int x_px, int y_px, int abilities);

/* Advance exactly one 60-Hz tick. pressed is a subset of logical actions
 * newly pressed this tick, independent of held (a quick tap may have ended).
 * Clears events first. Does not process record, jump-cut, interaction or
 * rewind, which the enclosing game handles at the documented tick boundary. */
void player_step(struct Player* p, struct Room* room, int held, int pressed);

/* Return an active clapper hitbox, or zero during idle/startup/recovery. */
int player_attack_box(struct Player* p, struct Hitbox* box);

/* Apply recoil and invulnerability once; return zero if already protected.
 * Call after player_step so EV_HURT survives for presentation that tick. */
int player_hurt(struct Player* p, int from_dir);

/* Check a destination against solid geometry and room bounds; one-way and
 * spike tiles are not safe teleport destinations when they overlap a body. */
int player_can_place(struct Room* room, int x, int y);

/* Move to a validated Q8 destination, clearing attachment and stale jump
 * state. Return zero without changes if the body would overlap a hazard or
 * solid tile. The game emits the jump-cut/rewind presentation event. */
int player_place(struct Player* p, struct Room* room, int x, int y);

/* Test positive-area rectangle overlap, used by plates and combat. */
int hitbox_overlap(struct Hitbox* a, struct Hitbox* b);

#endif
