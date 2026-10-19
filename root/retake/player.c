#include "player.h"

/* These are feel parameters in Q8 pixels per fixed 60-Hz tick. No hardware
 * timing or floating-point behavior is assumed by this simulation. */
#define RUN_SPEED 512
#define RUN_ACCEL 80
#define AIR_ACCEL 56
#define STOP_ACCEL 96
#define GRAVITY 56
#define HOLD_GRAVITY 40
#define APEX_GRAVITY 28
#define FALL_SPEED 896
#define FAST_FALL_SPEED 1280
#define JUMP_SPEED 1040
#define JUMP_CUT_SPEED 384
#define WALL_JUMP_SPEED 640
#define WALL_SLIDE_SPEED 192
#define GLIDE_SPEED 256
#define COYOTE_TICKS 6
#define BUFFER_TICKS 5
#define HOLD_TICKS 12
#define WALL_LOCK_TICKS 6
#define WALL_JUMP_REACH 3
#define DROP_TICKS 12
#define CLAP_TICKS 12
#define CLAP_ACTIVE_MIN 9
#define CLAP_COOLDOWN 18
#define HURT_TICKS 60
#define BODY_W 2048
#define BODY_H 3072
#define TILE_Q8 2048

/* Floor division is explicit for negative coordinates; native C does not
 * promise that right-shifting a negative signed integer is arithmetic. */
static int tile_index(int q) {
  if (q < 0) return -1 - ((-q - 1) >> 11);
  return q >> 11;
}

/* Bounds and unknown materials close the room rather than reading outside
 * caller-owned arrays or allowing entry into unrecognized terrain. */
static int tile_at(struct Room* r, int tx, int ty) {
  if (tx < 0 || ty < 0 || tx >= r->w || ty >= r->h) return COL_SOLID;
  int c = r->col[r->row[ty] + tx];
  if (c < COL_AIR || c > COL_SPIKE) return COL_SOLID;
  return c;
}

/* Test only positive-area contact, so standing exactly on a floor is clear. */
static int solid_body(struct Room* r, int x, int y) {
  int x0 = tile_index(x);
  int x1 = tile_index(x + BODY_W - 1);
  int y0 = tile_index(y);
  int y1 = tile_index(y + BODY_H - 1);
  for (int ty = y0; ty <= y1; ty++) {
    for (int tx = x0; tx <= x1; tx++) {
      if (tile_at(r, tx, ty) == COL_SOLID) return 1;
    }
  }
  return 0;
}

/* Approach a requested speed without overshoot or multiplication. */
static int approach(int value, int target, int amount) {
  if (value < target) {
    value += amount;
    if (value > target) value = target;
  } else if (value > target) {
    value -= amount;
    if (value < target) value = target;
  }
  return value;
}

/* Sweep every crossed tile column; a fast carry cannot tunnel through a
 * thin wall. Only solid tiles block horizontal motion. */
static int move_x(struct Player* p, struct Room* r, int dx) {
  if (!dx) return 1;
  int target = p->x + dx;
  int top = tile_index(p->y);
  int bottom = tile_index(p->y + BODY_H - 1);
  int step = dx > 0 ? 1 : -1;
  int first = dx > 0 ? tile_index(p->x + BODY_W) : tile_index(p->x - 1);
  int last = dx > 0 ? tile_index(target + BODY_W - 1) : tile_index(target);
  for (int tx = first; step > 0 ? tx <= last : tx >= last; tx += step) {
    for (int ty = top; ty <= bottom; ty++) {
      int c = tile_at(r, tx, ty);
      if (c == COL_SOLID) {
        p->x = step > 0 ? tx * TILE_Q8 - BODY_W : (tx + 1) * TILE_Q8;
        return 0;
      }
      if (c == COL_SPIKE) p->events |= EV_HAZARD;
    }
  }
  p->x = target;
  return 1;
}

/* Check an entire tile row for a blocking ceiling, without changing state. */
static int ceiling_row(struct Room* r, int x, int ty) {
  int right = tile_index(x + BODY_W - 1);
  for (int tx = tile_index(x); tx <= right; tx++) {
    if (tile_at(r, tx, ty) == COL_SOLID) return 1;
  }
  return 0;
}

/* A small sideways correction may clear a ceiling lip during ascent. Both
 * ends and every crossed ceiling row must be clear, preventing corrections
 * through walls or into a second ledge. The nearest correction wins. */
static int correct_corner(struct Player* p, struct Room* r, int target_y) {
  int first = tile_index(p->y - 1);
  int last = tile_index(target_y);
  for (int distance = 1; distance <= 3; distance++) {
    for (int side = -1; side <= 1; side += 2) {
      int x = p->x + distance * (side * Q8);
      int clear = !solid_body(r, x, p->y) && !solid_body(r, x, target_y);
      for (int ty = first; clear && ty >= last; ty--) {
        if (ceiling_row(r, x, ty)) clear = 0;
      }
      if (clear) {
        p->x = x;
        p->y = target_y;
        return 1;
      }
    }
  }
  return 0;
}

/* Sweep static geometry vertically. Return -1 for ceiling contact, 1 for
 * floor contact, or 0 for the full displacement. One-way floors require the
 * old feet to be at/above their top; upward motion always passes through. */
static int move_y(struct Player* p, struct Room* r, int dy, int correct) {
  if (!dy) return 0;
  int target = p->y + dy;
  int left = tile_index(p->x);
  int right = tile_index(p->x + BODY_W - 1);
  if (dy < 0) {
    int first = tile_index(p->y - 1);
    int last = tile_index(target);
    for (int ty = first; ty >= last; ty--) {
      if (ceiling_row(r, p->x, ty)) {
        if (correct && correct_corner(p, r, target)) return 0;
        p->y = (ty + 1) * TILE_Q8;
        return -1;
      }
    }
  } else {
    int feet = p->y + BODY_H;
    int first = tile_index(feet);
    int last = tile_index(target + BODY_H);
    for (int ty = first; ty <= last; ty++) {
      int top = ty * TILE_Q8;
      for (int tx = left; tx <= right; tx++) {
        int c = tile_at(r, tx, ty);
        if (c == COL_SOLID || (c == COL_ONEWAY && !p->drop_timer && feet <= top)) {
          p->y = top - BODY_H;
          return 1;
        }
      }
    }
  }
  p->y = target;
  return 0;
}

/* Determine which wall, if any, is flush against the body. */
static int wall_contact(struct Player* p, struct Room* r) {
  if (solid_body(r, p->x - 1, p->y)) return -1;
  if (solid_body(r, p->x + 1, p->y)) return 1;
  return 0;
}

/* Jump input may reach a nearby wall without granting support or a slide.
 * Check nearest first and favor facing on ties. The kick lock prevents a
 * second press from reusing the same wall before Ruby has separated. */
static int jump_wall(struct Player* p, struct Room* r) {
  if (p->wall_dir) return p->wall_dir;
  if (p->wall_lock) return 0;
  for (int distance = 1; distance <= WALL_JUMP_REACH; distance++) {
    int reach = distance * Q8 + 1;
    if (solid_body(r, p->x + p->facing * reach, p->y)) return p->facing;
    if (solid_body(r, p->x - p->facing * reach, p->y)) return -p->facing;
  }
  return 0;
}

/* Revalidate a previous attachment against the platform's old pose, then
 * apply its delta through ordinary tile sweeps. An obstructed carry drops
 * the attachment, while the immutable projection continues through scenery.
 * Return its id on obstruction so later probes cannot reacquire it this tick. */
static int carry_rider(struct Player* p, struct Room* r) {
  if (p->riding < 0) return -1;
  int blocked = -1;
  for (int i = 0; i < r->platform_count; i++) {
    struct Platform* s = &r->platforms[i];
    if (s->id != p->riding) continue;
    int old_x = s->x - s->dx;
    int old_y = s->y - s->dy;
    if (!s->active || p->drop_timer || p->y + BODY_H != old_y ||
        p->x + BODY_W <= old_x || p->x >= old_x + s->w) break;
    int complete_x = move_x(p, r, s->dx);
    int contact_y = move_y(p, r, s->dy, 0);
    if (complete_x && !contact_y) return -1;
    blocked = s->id;
    break;
  }
  p->riding = -1;
  return blocked;
}

/* Probe a one-Q8-unit descent without moving the player. The caller may
 * refill aerial abilities only when this returns real support. A carry that
 * was blocked earlier in the tick cannot immediately regain attachment. */
static int support(struct Player* p, struct Room* r, int blocked) {
  int saved_y = p->y;
  int hit = move_y(p, r, 1, 0);
  p->y = saved_y;
  if (hit == 1) {
    p->riding = -1;
    return 1;
  }
  if (!p->drop_timer) {
    for (int i = 0; i < r->platform_count; i++) {
      struct Platform* s = &r->platforms[i];
      if (s->active && s->id != blocked && p->y + BODY_H == s->y &&
          p->x + BODY_W > s->x && p->x < s->x + s->w) {
        p->riding = s->id;
        return 1;
      }
    }
  }
  p->riding = -1;
  return 0;
}

/* A downward press+jump drops through projected/one-way floors, but cannot
 * phase through a solid floor that happens to share the same height. */
static int can_drop(struct Player* p, struct Room* r) {
  int feet = p->y + BODY_H;
  int ty = tile_index(feet);
  int found = p->riding >= 0;
  for (int tx = tile_index(p->x); tx <= tile_index(p->x + BODY_W - 1); tx++) {
    int c = tile_at(r, tx, ty);
    if (c == COL_SOLID) return 0;
    if (c == COL_ONEWAY) found = 1;
  }
  return found;
}

/* Consume one buffered jump using ground grace, a wall, then the wire stunt.
 * A wall kick briefly locks horizontal input so holding into the wall cannot
 * immediately cancel its separating impulse. */
static int try_jump(struct Player* p, struct Room* r) {
  if (!p->jump_buffer) return 0;
  int wall = jump_wall(p, r);
  if (p->on_ground || p->coyote) {
    p->events |= EV_JUMP;
  } else if (wall) {
    p->vx = wall < 0 ? WALL_JUMP_SPEED : -WALL_JUMP_SPEED;
    p->facing = -wall;
    p->wall_lock = WALL_LOCK_TICKS;
    p->events |= EV_WALLJUMP;
  } else if ((p->abilities & AB_WIRE) && p->air_jump_left) {
    p->air_jump_left = 0;
    p->events |= EV_WIRE;
  } else {
    return 0;
  }
  p->vy = -JUMP_SPEED;
  p->on_ground = 0;
  p->riding = -1;
  p->coyote = 0;
  p->jump_buffer = 0;
  p->jump_hold = HOLD_TICKS;
  p->jump_cut = 1;
  return 1;
}

/* Moving surfaces catch only downward relative crossings from above. Test
 * the old and new surface heights, so an ascending take can catch a falling
 * player. Reject a catch that would embed the player in live solid geometry. */
static void land_platform(struct Player* p, struct Room* r, int old_feet, int blocked) {
  if (p->drop_timer) return;
  for (int i = 0; i < r->platform_count; i++) {
    struct Platform* s = &r->platforms[i];
    if (!s->active || s->id == blocked || p->x + BODY_W <= s->x || p->x >= s->x + s->w) continue;
    if (old_feet > s->y - s->dy || p->y + BODY_H < s->y) continue;
    int target_y = s->y - BODY_H;
    if (solid_body(r, p->x, target_y)) continue;
    p->y = target_y;
    p->vy = 0;
    p->on_ground = 1;
    p->riding = s->id;
  }
}

/* Hazard checks use the body interior, not its enclosing tile boundaries. */
static void check_hazards(struct Player* p, struct Room* r) {
  int right = tile_index(p->x + BODY_W - 1);
  int bottom = tile_index(p->y + BODY_H - 1);
  for (int ty = tile_index(p->y); ty <= bottom; ty++) {
    for (int tx = tile_index(p->x); tx <= right; tx++) {
      if (tile_at(r, tx, ty) == COL_SPIKE) p->events |= EV_HAZARD;
    }
  }
}

/* Pose priority makes recoil and clapper animation visible during jumps. */
static void update_pose(struct Player* p) {
  if (p->hurt_timer > HURT_TICKS - 8) p->pose = POSE_HURT;
  else if (p->attack_timer) p->pose = POSE_CLAP;
  else if (p->on_ground) p->pose = p->vx ? POSE_RUN : POSE_IDLE;
  else if (p->gliding) p->pose = POSE_GLIDE;
  else if (p->wall_dir && p->vy > 0) p->pose = POSE_WALL;
  else p->pose = p->vy < 0 ? POSE_RISE : POSE_FALL;
}

/* Initialize every scalar explicitly so native and guest traces agree. */
void player_init(struct Player* p, int x_px, int y_px, int abilities) {
  p->x = x_px * Q8;
  p->y = y_px * Q8;
  p->vx = 0;
  p->vy = 0;
  p->facing = 1;
  p->on_ground = 0;
  p->wall_dir = 0;
  p->abilities = abilities;
  p->air_jump_left = 1;
  p->gliding = 0;
  p->attack_timer = 0;
  p->attack_dir = 0;
  p->hurt_timer = 0;
  p->events = 0;
  p->pose = POSE_IDLE;
  p->riding = -1;
  p->edge_vx = 0;
  p->edge_vy = 0;
  p->coyote = 0;
  p->jump_buffer = 0;
  p->jump_hold = 0;
  p->jump_cut = 0;
  p->wall_lock = 0;
  p->attack_cooldown = 0;
  p->drop_timer = 0;
}

/* Resolve input once per fixed tick, then sweep movement, resolve support,
 * consume buffered landing jumps, and publish presentation state. */
void player_step(struct Player* p, struct Room* r, int held, int pressed) {
  int was_grounded = p->on_ground;
  int was_gliding = p->gliding;
  int old_feet = p->y + BODY_H;
  p->events = 0;
  p->edge_vx = 0;
  p->edge_vy = 0;
  p->gliding = 0;
  if (p->hurt_timer) p->hurt_timer--;
  if (p->attack_timer) p->attack_timer--;
  if (!p->attack_timer) p->attack_dir = 0;
  if (p->attack_cooldown) p->attack_cooldown--;
  if (p->wall_lock) p->wall_lock--;
  if (p->drop_timer) p->drop_timer--;
  if (pressed & IN_JUMP) p->jump_buffer = BUFFER_TICKS;

  int blocked = carry_rider(p, r);
  p->on_ground = p->vy >= 0 && support(p, r, blocked);
  p->wall_dir = wall_contact(p, r);
  if (p->on_ground) {
    p->coyote = COYOTE_TICKS;
    p->air_jump_left = 1;
    p->vy = 0;
  }
  if ((held & IN_DOWN) && (pressed & IN_JUMP) && p->on_ground && can_drop(p, r)) {
    p->drop_timer = DROP_TICKS;
    p->on_ground = 0;
    p->riding = -1;
    p->coyote = 0;
    p->jump_buffer = 0;
    p->vy = GRAVITY;
  }

  int direction = 0;
  if (held & IN_LEFT) direction--;
  if (held & IN_RIGHT) direction++;
  if (!p->wall_lock) {
    if (direction) p->facing = direction;
    int acceleration = p->on_ground ? RUN_ACCEL : AIR_ACCEL;
    if (!direction) acceleration = STOP_ACCEL;
    /* An intentional reversal brakes harder than a start from rest. This
     * reduces ledge overshoot without changing top speed or vertical jump arc. */
    else if ((direction < 0 && p->vx > 0) || (direction > 0 && p->vx < 0))
      acceleration += acceleration;
    p->vx = approach(p->vx, direction * RUN_SPEED, acceleration);
  }
  if ((pressed & IN_ATTACK) && !p->attack_cooldown) {
    p->attack_timer = CLAP_TICKS;
    p->attack_cooldown = CLAP_COOLDOWN;
    p->attack_dir = (held & IN_UP) ? 2 : ((held & IN_DOWN) ? 3 : 1);
    p->events |= EV_CLAP;
  }
  int jumped = try_jump(p, r);
  if (!(held & IN_JUMP) && p->jump_cut) {
    if (p->vy < -JUMP_CUT_SPEED) p->vy = -JUMP_CUT_SPEED;
    p->jump_hold = 0;
    p->jump_cut = 0;
  }
  if (!p->on_ground) {
    int gravity = GRAVITY;
    if ((held & IN_JUMP) && p->jump_hold) gravity = HOLD_GRAVITY;
    else if ((held & IN_JUMP) && p->vy > -160 && p->vy < 160) gravity = APEX_GRAVITY;
    p->vy += gravity;
    int cap = (held & IN_DOWN) ? FAST_FALL_SPEED : FALL_SPEED;
    if (p->wall_dir && direction == p->wall_dir) cap = WALL_SLIDE_SPEED;
    if ((p->abilities & AB_GLIDE) && (held & IN_JUMP) && p->vy > 0) {
      p->gliding = 1;
      if (cap > GLIDE_SPEED) cap = GLIDE_SPEED;
    }
    if (p->vy > cap) p->vy = cap;
  }

  if (!move_x(p, r, p->vx)) {
    if ((p->vx < 0 && p->x == 0) ||
        (p->vx > 0 && p->x + BODY_W == r->w * TILE * Q8)) p->edge_vx = p->vx;
    p->vx = 0;
  }
  int vertical_hit = move_y(p, r, p->vy, p->vy < 0);
  p->on_ground = vertical_hit == 1;
  if (vertical_hit) {
    if ((p->vy < 0 && p->y == 0) ||
        (p->vy > 0 && p->y + BODY_H == r->h * TILE * Q8)) p->edge_vy = p->vy;
    p->vy = 0;
    p->jump_hold = 0;
  }
  if (p->vy >= 0) p->jump_cut = 0;
  if (!jumped && p->vy >= 0) land_platform(p, r, old_feet, blocked);
  if (p->vy >= 0) p->on_ground = support(p, r, blocked);
  else p->riding = -1;
  p->wall_dir = wall_contact(p, r);
  if (p->on_ground) {
    p->vy = 0;
    p->coyote = COYOTE_TICKS;
    p->air_jump_left = 1;
    p->gliding = 0;
    if (!was_grounded) p->events |= EV_LAND;
    try_jump(p, r);
  } else if (p->coyote) {
    p->coyote--;
  }
  /* As with landing, a buffered wall jump belongs to the contact tick.
   * Consume it before the buffer expires; movement resumes next tick. */
  if (!p->on_ground && !jumped && p->jump_buffer) try_jump(p, r);
  if (p->jump_buffer) p->jump_buffer--;
  if (p->jump_hold) p->jump_hold--;
  if (p->gliding && !was_gliding) p->events |= EV_GLIDE;
  check_hazards(p, r);
  update_pose(p);
}

/* Common live/projected attack geometry; directed claps extend beyond the
 * body without making the player itself a damaging contact rectangle. */
void clapper_box(int x, int y, int facing, int dir, struct Hitbox* b) {
  b->w = 12 * Q8;
  b->h = 12 * Q8;
  b->x = x - 2 * Q8;
  b->y = y - 12 * Q8;
  if (dir == 1) {
    b->w = 14 * Q8;
    b->h = 10 * Q8;
    b->x = facing > 0 ? x + BODY_W : x - 14 * Q8;
    b->y = y + Q8;
  } else if (dir == 3) {
    b->y = y + BODY_H;
  }
}

/* The snap hits immediately for four ticks, followed by visible recovery. */
int player_attack_box(struct Player* p, struct Hitbox* b) {
  if (p->attack_timer < CLAP_ACTIVE_MIN || !p->attack_dir) return 0;
  clapper_box(p->x, p->y, p->facing, p->attack_dir, b);
  return 1;
}

/* Recoil is applied only once per invulnerability window. */
int player_hurt(struct Player* p, int from_dir) {
  if (p->hurt_timer) return 0;
  p->hurt_timer = HURT_TICKS;
  p->vx = from_dir < 0 ? RUN_SPEED : -RUN_SPEED;
  p->vy = -512;
  p->wall_lock = 8;
  p->on_ground = 0;
  p->riding = -1;
  p->coyote = 0;
  p->jump_hold = 0;
  p->jump_cut = 0;
  p->jump_buffer = 0;
  p->attack_timer = 0;
  p->attack_dir = 0;
  p->events |= EV_HURT;
  p->pose = POSE_HURT;
  return 1;
}

/* Forced bounces own their vertical impulse and cannot retain stale support. */
void player_launch(struct Player* p, int speed) {
  p->vy = -speed;
  p->on_ground = 0;
  p->riding = -1;
  p->wall_dir = 0;
  p->coyote = 0;
  p->jump_buffer = 0;
  p->jump_hold = 0;
  p->jump_cut = 0;
  p->drop_timer = 0;
  p->air_jump_left = 1;
  p->gliding = 0;
  p->pose = POSE_RISE;
}

/* Placement deliberately excludes hazards and one-way interiors, even
 * though normal movement can pass through them in some directions. */
int player_can_place(struct Room* r, int x, int y) {
  int right = tile_index(x + BODY_W - 1);
  int bottom = tile_index(y + BODY_H - 1);
  for (int ty = tile_index(y); ty <= bottom; ty++) {
    for (int tx = tile_index(x); tx <= right; tx++) {
      if (tile_at(r, tx, ty) != COL_AIR) return 0;
    }
  }
  return 1;
}

/* A successful cut keeps momentum, while preventing stale support or
 * buffered input from creating an unintended extra jump at the destination. */
int player_place(struct Player* p, struct Room* r, int x, int y) {
  if (!player_can_place(r, x, y)) return 0;
  p->x = x;
  p->y = y;
  p->riding = -1;
  p->on_ground = 0;
  p->wall_dir = 0;
  p->coyote = 0;
  p->jump_buffer = 0;
  p->jump_hold = 0;
  p->jump_cut = 0;
  p->drop_timer = 0;
  return 1;
}

/* Zero-sized boxes and touching edges do not count as hits. */
int hitbox_overlap(struct Hitbox* a, struct Hitbox* b) {
  return a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0 &&
    a->x < b->x + b->w && b->x < a->x + a->w &&
    a->y < b->y + b->h && b->y < a->y + a->h;
}
