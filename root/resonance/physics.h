#ifndef RESONANCE_PHYSICS_H
#define RESONANCE_PHYSICS_H

/*
 * Deterministic musical-pinball model, shared by the Dioptase user program
 * and native correctness tests. All positions, radii, segment lengths, and
 * velocities are signed Q8 pixels (256 units/pixel); velocity is per 60 Hz
 * tick. Normals and restitution also use Q8. Table dimensions alone are
 * whole pixels. The supported bounds keep all intermediates within signed
 * 32-bit arithmetic: no floating point or 64-bit type is required.
 *
 * One owner calls res_step and reads its completed state on the same thread.
 * This module performs no syscalls, privileged operations, allocation, or
 * MMIO. The guest compiler supplies the documented ABI (docs/abi.md Types
 * and Registers); there is no custom stack, TLS, or register convention.
 * The table and geometry arrays are borrowed immutable storage and must
 * outlive the game. Rendering/audio consume events after a step and never
 * mutate the model. Events are cleared by the next step, not by the reader.
 */

#define RES_FP_SHIFT 8
#define RES_ONE 256
#define RES_TICK_HZ 60
#define RES_MAX_BALLS 4
#define RES_MAX_COLLIDERS 128
#define RES_MAX_EVENTS 32
#define RES_FLIP_POSES 17
#define RES_MAX_TABLE_SIZE 1024

/* Held controls; nudge is acted upon only on a rising edge. */
#define RES_INPUT_LEFT 1u
#define RES_INPUT_RIGHT 2u
#define RES_INPUT_PLUNGER 4u
#define RES_INPUT_NUDGE 8u

/* The segment endpoints define its tangent; normal is (-dy, dx)/length. */
struct ResSegment {
  int x1;
  int y1;
  int x2;
  int y2;
  int nx;
  int ny;
  int length;
  int radius;
};

/* Shapes select collision geometry; kinds select scoring and kick behavior. */
enum ResShape {
  RES_SHAPE_SEGMENT,
  RES_SHAPE_CIRCLE
};

/* Lanes and saucers are circular sensors; gates are one-sided segments. */
enum ResKind {
  RES_WALL,
  RES_BUMPER,
  RES_SLING,
  RES_TARGET,
  RES_LANE,
  RES_SAUCER,
  RES_GATE
};

/* Circle geometry uses x1/y1/radius; other segment fields are zero. */
struct ResCollider {
  int shape;
  int kind;
  int tag;
  struct ResSegment geo;
  int restitution;
  int kick;
};

/* The artwork generator supplies these poses for collision and rendering. */
struct ResFlipper {
  struct ResSegment poses[RES_FLIP_POSES];
};

/* Geometry and launch/ejection points belong to the table's single source. */
struct ResTable {
  int width;
  int height;
  const struct ResCollider* colliders;
  int collider_count;
  struct ResFlipper flippers[2];
  int shooter_x;
  int shooter_y;
  int eject_x;
  int eject_y;
  int drain_y;
};

/* Captured balls wait for saucer ejection; shooter balls wait for release. */
enum ResBallState {
  RES_BALL_OFF,
  RES_BALL_LIVE,
  RES_BALL_SHOOTER,
  RES_BALL_CAPTURED
};

/* A ball's timers and contact cooldowns are private to the simulation. */
struct ResBall {
  int state;
  int x;
  int y;
  int vx;
  int vy;
  int timer;
  int last_hit;
  unsigned char cooldown[RES_MAX_COLLIDERS];
};

/* Events describe completed gameplay facts, in deterministic production order. */
enum ResEventKind {
  RES_EVENT_HIT,
  RES_EVENT_FLIPPER,
  RES_EVENT_LAUNCH,
  RES_EVENT_DRAIN,
  RES_EVENT_SAVE,
  RES_EVENT_BANK,
  RES_EVENT_LANES,
  RES_EVENT_CAPTURE,
  RES_EVENT_MULTIBALL,
  RES_EVENT_JACKPOT,
  RES_EVENT_NUDGE,
  RES_EVENT_TILT,
  RES_EVENT_GAME_OVER
};

/* Collider is its table index, or -1 for events without a static collider. */
struct ResEvent {
  int kind;
  int collider;
  int ball;
  int x;
  int y;
  int strength;
};

/* The model exposes presentation state without depending on a renderer. */
struct ResGame {
  const struct ResTable* table;
  struct ResBall balls[RES_MAX_BALLS];
  struct ResEvent events[RES_MAX_EVENTS];
  int event_count;
  unsigned dropped_events;
  unsigned tick;
  unsigned rng;
  unsigned previous_input;
  unsigned score;
  int ball_number;
  int balls_remaining;
  int game_over;
  int flipper_pose[2];
  int plunger_charge;
  int save_ticks;
  int tilt_heat;
  int tilted;
  int target_mask;
  int lane_mask;
  int multiplier;
  int combo;
  int combo_ticks;
  int resonance_charge;
  int saucer_lit;
  int multiball;
  int serve_ticks;
  int lamp_ticks[RES_MAX_COLLIDERS];
};

/* Validate immutable geometry and start a three-ball game; 0 means success. */
int res_init(struct ResGame* game, const struct ResTable* table, unsigned seed);

/* Advance exactly one 60 Hz tick; the caller owns elapsed-time catch-up. */
void res_step(struct ResGame* game, unsigned input);

/* Return deterministic held controls for the title-screen demonstration. */
unsigned res_autoplay(const struct ResGame* game);

/* Count live/captured balls, excluding balls awaiting the plunger. */
int res_active_balls(const struct ResGame* game);

#endif
