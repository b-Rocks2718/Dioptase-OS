#ifndef RETAKE_TAKES_H
#define RETAKE_TAKES_H

#include "player.h"

#define TAKE_MAX_SLOTS 3
#define TAKE_MAX_FRAMES 480
#define TAKE_BASE_FRAMES 240

/* A projected frame records resolved motion rather than input. attack_dir
 * is nonzero only for active clapper frames. The recording does not change
 * when doors close, an enemy moves, or the live player stands on a double. */
struct TakeFrame {
  int x;
  int y;
  int facing;
  int pose;
  int attack_dir;
};

/* One looping projection. cursor names the displayed frame; wrapped means
 * it just returned to frame zero and cannot carry/support a rider this tick.
 * dx/dy describe only continuous playback; large jumps also set wrapped.
 * fresh is private state for a newly published slot. A single-frame still
 * remains continuously supportive after its initial flicker. */
struct Take {
  struct TakeFrame frames[480];
  int length;
  int cursor;
  int active;
  int wrapped;
  int dx;
  int dy;
  int fresh;
};

/* Caller-owned room-local recordings: three playback slots and one scratch
 * take being recorded. Limits may be upgraded but never exceed the maxima.
 * Completing a recording uses an empty slot, then replaces slots cyclically.
 * Clear this state on room transitions, death/respawn, or a new game. */
struct Takes {
  struct Take slots[3];
  struct TakeFrame scratch[480];
  int recording;
  int recorded;
  int slot_limit;
  int frame_limit;
  int replace_next;
};

/* Clear playback and recording, clamping upgrade limits to valid ranges.
 * Frame buffers need not be initialized: length controls all reads. */
void takes_init(struct Takes* t, int slots, int frames);

/* At the start of a simulation tick, advance existing projections before
 * deriving platforms and stepping the live player. Newly recorded takes
 * start at frame zero on their first advance. */
void takes_advance(struct Takes* t);

/* Fill up to three caller-owned platform entries in slot order, preserving
 * stable ids even when a slot is empty or wrapping. Return the entry count.
 * Use this list as Room.platforms for the subsequent player_step. */
int takes_platforms(struct Takes* t, struct Platform* out);

/* After player_step, capture its resolved pose if recording. A record press
 * starts with this frame, or finishes the existing recording including this
 * frame. Reaching frame_limit finishes automatically. Return 1 on start,
 * 2 on finish, 0 otherwise. Single-frame takes are valid stationary stands. */
int takes_capture(struct Takes* t, struct Player* p, int record_pressed);

/* Cancel only the in-progress recording; existing playback remains intact. */
void takes_cancel(struct Takes* t);

/* Build the clapper hitbox of the currently projected frame, if active.
 * The game uses its ordinary enemy hurt cooldown for live and filmed hits. */
int take_attack_box(struct Take* t, struct Hitbox* box);

/* Attempt a jump cut to one displayed take. Reject empty, wrapping, or
 * blocked destinations; failed cuts leave the player unchanged. Ability
 * unlocks, selection and cooldown belong to the enclosing game. */
int takes_jumpcut(struct Takes* t, struct Player* p, struct Room* room, int slot);

#endif
