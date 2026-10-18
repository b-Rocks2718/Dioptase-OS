#ifndef RETAKE_PLAYER_H
#define RETAKE_PLAYER_H

/*
 * Movement-core contract between the game (main loop, renderer, entities)
 * and the player controller. Agreed between Opus and Astra; player.c
 * implements it. Positions and velocities are Q8 fixed point in room pixels
 * (256 = one pixel); the controller runs at a fixed 60 Hz.
 */

#define TILE 8

/* Collision codes stored per tile in Room.col. */
#define COL_AIR 0
#define COL_SOLID 1
#define COL_ONEWAY 2   /* solid from above only, while falling with feet at or above its top */
#define COL_SPIKE 3    /* hazard: touching it raises EV_HAZARD */

/* Input bits, passed as a held mask and a pressed-this-tick mask. */
#define IN_LEFT 1
#define IN_RIGHT 2
#define IN_UP 4
#define IN_DOWN 8
#define IN_JUMP 16
#define IN_DASH 32
#define IN_SLASH 64
#define IN_RECORD 128
#define IN_CUT 256

/* Ability bits. */
#define AB_DASH 1
#define AB_FLUTTER 2

/* Event bits raised by player_step for the tick just simulated. */
#define EV_JUMP 1
#define EV_WALLJUMP 2
#define EV_LAND 4
#define EV_DASH 8
#define EV_FLUTTER 16
#define EV_HAZARD 32
#define EV_SLASH 64
#define EV_POGO 128
#define EV_REFILL 256

/* Body hitbox in pixels; Player.x/y is its top-left corner. */
#define BODY_W 8
#define BODY_H 14

/* A room's collision view. row[ty] == ty * w, precomputed so the controller
 * never multiplies on its hot path (software multiply is slow on Dioptase). */
struct Room {
  int w;
  int h;
  unsigned char* col;
  int* row;
};

struct Player {
  int x;
  int y;
  int vx;
  int vy;
  int facing;       /* -1 left, +1 right */
  int on_ground;
  int wall_dir;     /* -1/+1 while touching a wall in the air, else 0 */
  int dashing;
  int dashes_left;
  int abilities;
  int slash_timer;
  int slash_dir;    /* 0 none, 1 side, 2 up, 3 down */
  int hurt_timer;
  unsigned events;
  /* private controller state */
  int coyote;
  int jump_buffer;
  int jump_hold;
  int wall_lock;
};

void player_init(struct Player* p, int x_px, int y_px, int abilities);
void player_step(struct Player* p, struct Room* room, int held, int pressed);
void player_pogo(struct Player* p);
void player_refill(struct Player* p);

#endif
