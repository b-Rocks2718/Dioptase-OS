#include "player.h"

/*
 * STUB movement controller (Opus). It exists so rendering, camera and room
 * work can proceed before Astra's tuned and host-tested controller lands; it
 * implements the player.h contract with plain, untuned physics and will be
 * replaced wholesale. All arithmetic is shifts and adds: no multiplies.
 */

#define RUN_SPEED 384      /* 1.5 px/tick */
#define GROUND_ACCEL 64
#define AIR_ACCEL 40
#define GRAVITY 64         /* 0.25 px/tick^2 */
#define MAX_FALL 680
#define WALL_SLIDE 160
#define JUMP_SPEED 456
#define COYOTE_TICKS 6
#define BUFFER_TICKS 5
#define WALL_LOCK_TICKS 8
#define SLASH_TICKS 12

/* Collision code at tile (tx, ty); outside the room counts as solid on the
 * sides and air above and below, so rooms can open upward and downward. */
static int tile_at(struct Room* r, int tx, int ty) {
  if (tx < 0 || tx >= r->w) return COL_SOLID;
  if (ty < 0 || ty >= r->h) return COL_AIR;
  return r->col[r->row[ty] + tx];
}

/* Whether any tile under the pixel rectangle [x0, x1) x [y0, y1) is solid. */
static int rect_solid(struct Room* r, int x0, int y0, int x1, int y1) {
  int tx0 = x0 >> 3;
  int tx1 = (x1 - 1) >> 3;
  int ty0 = y0 >> 3;
  int ty1 = (y1 - 1) >> 3;
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tile_at(r, tx, ty) == COL_SOLID) return 1;
    }
  }
  return 0;
}

/* Whether the body overlaps a spike tile. */
static int touches_spike(struct Room* r, int px, int py) {
  int tx0 = (px + 1) >> 3;
  int tx1 = (px + BODY_W - 2) >> 3;
  int ty0 = (py + 2) >> 3;
  int ty1 = (py + BODY_H - 1) >> 3;
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tile_at(r, tx, ty) == COL_SPIKE) return 1;
    }
  }
  return 0;
}

/* One-way platform directly under the feet at pixel row `feet`. */
static int oneway_below(struct Room* r, int px, int feet) {
  if ((feet & 7) != 0) return 0;
  int ty = feet >> 3;
  int tx0 = px >> 3;
  int tx1 = (px + BODY_W - 1) >> 3;
  for (int tx = tx0; tx <= tx1; tx++) {
    if (tile_at(r, tx, ty) == COL_ONEWAY) return 1;
  }
  return 0;
}

void player_init(struct Player* p, int x_px, int y_px, int abilities) {
  p->x = x_px << 8;
  p->y = y_px << 8;
  p->vx = 0;
  p->vy = 0;
  p->facing = 1;
  p->on_ground = 0;
  p->wall_dir = 0;
  p->dashing = 0;
  p->dashes_left = 1;
  p->abilities = abilities;
  p->slash_timer = 0;
  p->slash_dir = 0;
  p->hurt_timer = 0;
  p->events = 0;
  p->coyote = 0;
  p->jump_buffer = 0;
  p->jump_hold = 0;
  p->wall_lock = 0;
}

void player_pogo(struct Player* p) {
  p->vy = -JUMP_SPEED;
  p->events |= EV_POGO;
}

void player_refill(struct Player* p) {
  p->dashes_left = 1;
  p->events |= EV_REFILL;
}

void player_step(struct Player* p, struct Room* r, int held, int pressed) {
  p->events = 0;
  int dir = 0;
  if (held & IN_LEFT) dir -= 1;
  if (held & IN_RIGHT) dir += 1;
  if (p->wall_lock > 0) {
    p->wall_lock--;
    dir = 0;
  }
  if (dir != 0) p->facing = dir;

  /* horizontal acceleration toward the target run speed */
  int target = 0;
  if (dir > 0) target = RUN_SPEED;
  if (dir < 0) target = -RUN_SPEED;
  int accel = p->on_ground ? GROUND_ACCEL : AIR_ACCEL;
  if (p->vx < target) {
    p->vx += accel;
    if (p->vx > target) p->vx = target;
  } else if (p->vx > target) {
    p->vx -= accel;
    if (p->vx < target) p->vx = target;
  }

  /* jump buffering, coyote time and variable height */
  if (pressed & IN_JUMP) p->jump_buffer = BUFFER_TICKS;
  else if (p->jump_buffer > 0) p->jump_buffer--;
  if (p->on_ground) p->coyote = COYOTE_TICKS;
  else if (p->coyote > 0) p->coyote--;

  if (p->jump_buffer > 0 && p->coyote > 0) {
    p->vy = -JUMP_SPEED;
    p->jump_buffer = 0;
    p->coyote = 0;
    p->on_ground = 0;
    p->events |= EV_JUMP;
  } else if (p->jump_buffer > 0 && p->wall_dir != 0) {
    p->vy = -JUMP_SPEED;
    p->vx = -p->wall_dir * RUN_SPEED;
    p->facing = -p->wall_dir;
    p->wall_lock = WALL_LOCK_TICKS;
    p->jump_buffer = 0;
    p->wall_dir = 0;
    p->events |= EV_WALLJUMP;
  }
  if (!(held & IN_JUMP) && p->vy < 0) {
    p->vy += GRAVITY; /* released early: fall back sooner */
  }

  p->vy += GRAVITY;
  int max_fall = MAX_FALL;
  if (p->wall_dir != 0 && p->vy > 0) max_fall = WALL_SLIDE;
  if (p->vy > max_fall) p->vy = max_fall;

  /* attack */
  if (p->slash_timer > 0) p->slash_timer--;
  if ((pressed & IN_SLASH) && p->slash_timer == 0) {
    p->slash_timer = SLASH_TICKS;
    p->slash_dir = 1;
    if (held & IN_UP) p->slash_dir = 2;
    else if ((held & IN_DOWN) && !p->on_ground) p->slash_dir = 3;
    p->events |= EV_SLASH;
  }

  /* move X, one pixel at a time when blocked so contact is exact */
  int px = p->x >> 8;
  int py = p->y >> 8;
  int nx = (p->x + p->vx) >> 8;
  while (nx != px) {
    int step = nx > px ? 1 : -1;
    if (rect_solid(r, px + step, py, px + step + BODY_W, py + BODY_H)) {
      p->vx = 0;
      p->x = px << 8;
      nx = px;
      break;
    }
    px += step;
  }
  if (nx == px) p->x += p->vx;
  px = p->x >> 8;

  /* move Y */
  int ny = (p->y + p->vy) >> 8;
  int was_ground = p->on_ground;
  p->on_ground = 0;
  while (ny != py) {
    int step = ny > py ? 1 : -1;
    int blocked = rect_solid(r, px, py + step, px + BODY_W, py + step + BODY_H);
    if (!blocked && step > 0 && oneway_below(r, px, py + BODY_H)) blocked = 1;
    if (blocked) {
      if (step > 0) p->on_ground = 1;
      p->vy = 0;
      p->y = py << 8;
      ny = py;
      break;
    }
    py += step;
  }
  if (ny == py && p->vy != 0) p->y += p->vy;
  py = p->y >> 8;
  if (!p->on_ground && p->vy >= 0) {
    if (rect_solid(r, px, py + 1, px + BODY_W, py + 1 + BODY_H) || oneway_below(r, px, py + BODY_H)) {
      p->on_ground = 1;
    }
  }
  if (p->on_ground && !was_ground) p->events |= EV_LAND;

  /* wall contact for slides and wall jumps */
  p->wall_dir = 0;
  if (!p->on_ground) {
    if ((held & IN_RIGHT) && rect_solid(r, px + 1, py, px + 1 + BODY_W, py + BODY_H)) p->wall_dir = 1;
    if ((held & IN_LEFT) && rect_solid(r, px - 1, py, px - 1 + BODY_W, py + BODY_H)) p->wall_dir = -1;
  }

  if (touches_spike(r, px, py)) p->events |= EV_HAZARD;
}
