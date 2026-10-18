#include "../crt/sys.h"
#include "../crt/print.h"
#include "retake.h"
#include "game.h"
#include "pak.h"

/*
 * Game rules above the movement core (see game.h for the tick order).
 *
 * Rooms live in per-region world coordinates (pak room wx/wy, in tiles). When
 * Ruby presses against or falls through a room edge, the room of the same
 * region that contains the point just beyond that edge is loaded and she
 * keeps her world position. An edge with no neighbor is a wall, except the
 * bottom edge, which is a pit (a hazard).
 *
 * Takes are room-local and cleared on every room change and respawn, as
 * takes.h requires. Persistent pickups carry a flag id assigned by
 * tools/build.py; a set flag means "already collected".
 */

#define SAVE_PATH "/retake/save.dat"
#define SAVE_MAGIC 0x31565352      /* 'RSV1' */

#define HURT_INVULN 12
#define POGO_SPEED 900
#define SPRING_SPEED 1500
#define WALKER_SPEED 112
#define CHARGER_SPEED 640
#define HOPPER_JUMP 900
#define ENT_GRAVITY 48
#define ENT_FALL_MAX 1024
#define SHOT_TICKS 180
#define BOSS1_HP 16
#define HORSE_HP 10
#define WAVE_SPEED 640

struct Player g_hero;
struct Takes g_takes;
struct Progress g_prog;
struct Ent g_ents[40];
int g_ent_count;
int g_health;
unsigned g_events;
int g_pending_script = -1;
int g_pending_shop;
int g_switch[16];

struct Ent* g_boss;
#define MAX_MOVERS 8
static struct Platform g_platforms[12];   /* takes first, then moving platforms (ids 8+) */
static int entry_x;            /* where Ruby entered this room (pixels) */
static int entry_y;
static int room_ox;            /* this room's world origin in pixels */
static int room_oy;

/* ---- persistent flags ---- */

int flag_get(int id) {
  if (id < 0 || id >= MAX_FLAGS) return 0;
  return (g_prog.flags[id >> 5] >> (id & 31)) & 1;
}

void flag_set(int id) {
  if (id < 0 || id >= MAX_FLAGS) return;
  g_prog.flags[id >> 5] |= 1u << (id & 31);
}

static void mark_visited(int room) {
  g_prog.visited[(room >> 5) & 7] |= 1u << (room & 31);
}

/* ---- tile queries for entities (pixel coordinates) ---- */

static int col_px(int x, int y) {
  if (x < 0 || y < 0 || x >= g_room_pw || y >= g_room_ph) return COL_SOLID;
  return g_room.col[g_room.row[y >> 3] + (x >> 3)];
}

static int solid_px(int x, int y) {
  return col_px(x, y) == COL_SOLID;
}

static int floor_px(int x, int y) {
  int c = col_px(x, y);
  return c == COL_SOLID || c == COL_ONEWAY;
}

static void ent_box(struct Ent* e, struct Hitbox* b) {
  b->x = e->x;
  b->y = e->y;
  b->w = e->w;
  b->h = e->h;
}

static void hero_box(struct Hitbox* b) {
  b->x = g_hero.x;
  b->y = g_hero.y;
  b->w = PLAYER_W * Q8;
  b->h = PLAYER_H * Q8;
}

/* ---- entities ---- */

static int is_enemy(int kind) {
  return kind == ENT_WALKER || kind == ENT_HOPPER || kind == ENT_FLYER || kind == ENT_THROWER ||
         kind == ENT_CHARGER;
}

static struct Ent* ent_alloc(void) {
  for (int i = 0; i < g_ent_count; i++) {
    if (!g_ents[i].alive && g_ents[i].kind == ENT_NONE) return &g_ents[i];
  }
  if (g_ent_count >= MAX_ENTS) return 0;
  return &g_ents[g_ent_count++];
}

/* Initialize an entity of `kind` at tile (tx, ty) with its hitbox resting on
 * the tile's bottom edge. */
static void ent_setup(struct Ent* e, int kind, int tx, int ty, int a, int b, int c, int flag) {
  e->kind = kind;
  e->d = 0;
  e->tx = tx;
  e->ty = ty;
  e->a = a;
  e->b = b;
  e->c = c;
  e->flag = flag;
  e->alive = 1;
  e->vx = 0;
  e->vy = 0;
  e->hp = 1;
  e->state = 0;
  e->timer = 0;
  e->dir = -1;
  e->hurt = 0;
  e->anim = 0;
  int w = 8;
  int h = 8;
  switch (kind) {
    case ENT_COIN: w = 6; h = 6; break;
    case ENT_HEART: w = 8; h = 8; break;
    case ENT_FRAME: w = 10; h = 10; break;
    case ENT_REEL: w = 10; h = 10; break;
    case ENT_ABILITY: w = 12; h = 12; break;
    case ENT_MIRROR: w = 12; h = 22; break;
    case ENT_SIGN: w = 12; h = 12; break;
    case ENT_NPC: w = 10; h = 16; break;
    case ENT_PLATE: w = 8; h = 3; break;
    case ENT_GATE: w = 8; h = 8 * (b > 0 ? b : 1); break;
    case ENT_DOOR: w = 14; h = 24; break;
    case ENT_WALKER: w = 10; h = 14; e->hp = 2; break;
    case ENT_HOPPER: w = 10; h = 10; e->hp = 2; break;
    case ENT_FLYER: w = 10; h = 8; e->hp = 1; break;
    case ENT_THROWER: w = 10; h = 14; e->hp = 2; break;
    case ENT_CHARGER: w = 14; h = 10; e->hp = 3; break;
    case ENT_SHOT: w = 6; h = 6; break;
    case ENT_SANDBAG: w = 8; h = 8; break;
    case ENT_SPRING: w = 8; h = 4; break;
    case ENT_SHOP: w = 16; h = 16; break;
    case ENT_WAVE: w = 8; h = 8; break;
    case ENT_PLATFORM: w = 24; h = 4; break;   /* width fixed up from d after setup */
    case ENT_BOSS:
      if (a == 2) {
        w = 64;
        h = 30;
        e->hp = HORSE_HP;
      } else {
        w = 22;
        h = 30;
        e->hp = BOSS1_HP;
      }
      break;
    default: break;
  }
  e->w = w << 8;
  e->h = h << 8;
  /* stand on the bottom of the spawn tile, centered */
  e->x = ((tx << 3) + 4 - (w >> 1)) << 8;
  e->y = ((ty << 3) + 8 - h) << 8;
  if (kind == ENT_GATE) {
    e->x = (tx << 3) << 8;
    e->y = (ty << 3) << 8;
  }
  if (kind == ENT_SANDBAG) e->y = (ty << 3) << 8;
  if (kind == ENT_FLYER) {
    e->c = e->y;   /* home height */
  }
}

static void spawn_room_entities(void) {
  struct PakRoom* pr = pak_room(g_room_index);
  short* raw = (short*)(g_pak + pr->entities_off);
  g_ent_count = 0;
  g_boss = 0;
  for (int i = 0; i < pr->entity_count && g_ent_count < MAX_ENTS; i++) {
    short* r = raw + (i << 3);
    int kind = r[0];
    int flag = r[7];
    if (kind <= ENT_NONE || kind >= ENT_KINDS) continue;
    if (flag >= 0 && flag_get(flag)) continue;   /* already collected */
    if (kind == ENT_ABILITY && (g_prog.abilities & r[3])) continue;
    if (kind == ENT_BOSS && flag_get(BOSS_FLAG_BASE + r[3])) continue;
    struct Ent* e = &g_ents[g_ent_count++];
    ent_setup(e, kind, r[1], r[2], r[3], r[4], r[5], flag);
    e->d = r[6];
    if (kind == ENT_BOSS) g_boss = e;
    if (kind == ENT_PLATFORM) {
      int tiles = e->d > 0 ? e->d : 3;
      e->w = (tiles << 3) << 8;
      e->x = (r[1] << 3) << 8;
      e->y = (r[2] << 3) << 8;    /* the riding surface is the tile's top edge */
      e->c = e->c > 0 ? e->c : 128;
      e->dir = 1;
      e->state = 0;                /* distance travelled along the axis, Q8 */
    }
  }
}

/* Apply gates to the room's collision: a closed gate's column is solid. */
static void update_gates(void) {
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (e->kind != ENT_GATE) continue;
    int open = g_switch[e->a & 15];
    int want = open ? 1 : 0;
    if (want != e->state || e->timer == 0) {
      int h = e->b > 0 ? e->b : 1;
      for (int k = 0; k < h; k++) {
        int ty = e->ty + k;
        if (ty >= 0 && ty < g_room.h) g_room.col[g_room.row[ty] + e->tx] = open ? COL_AIR : COL_SOLID;
      }
      if (e->timer != 0) g_events |= GE_GATE;
      e->state = want;
      e->timer = 1;
    }
  }
}

/* ---- room changes ---- */

void game_enter_room(int room, int x_px, int y_px) {
  room_load(room);
  struct PakRoom* pr = pak_room(room);
  room_ox = pr->wx << 3;
  room_oy = pr->wy << 3;
  render_region(pr->region);
  int vx = g_hero.vx;
  int vy = g_hero.vy;
  int facing = g_hero.facing;
  int abilities = g_prog.abilities;
  player_init(&g_hero, x_px, y_px, abilities);
  g_hero.vx = vx;
  g_hero.vy = vy;
  g_hero.facing = facing;
  takes_init(&g_takes, g_prog.take_slots, g_prog.take_frames);
  for (int i = 0; i < MAX_SWITCHES; i++) g_switch[i] = 0;
  spawn_room_entities();
  update_gates();
  entry_x = x_px;
  entry_y = y_px;
  mark_visited(room);
  g_events |= GE_ROOM;
}

/* Find the room of `region` containing world pixel (wx, wy), or -1. */
static int room_at(int region, int wx, int wy) {
  int n = pak_room_count();
  for (int i = 0; i < n; i++) {
    struct PakRoom* r = pak_room(i);
    if (r->region != region) continue;
    int x0 = r->wx << 3;
    int y0 = r->wy << 3;
    if (wx >= x0 && wy >= y0 && wx < x0 + (r->w << 3) && wy < y0 + (r->h << 3)) return i;
  }
  return -1;
}

/* Hazard: lose a frame and return to where Ruby entered the room. */
static void hazard_respawn(void) {
  g_health--;
  g_events |= GE_HAZARD;
  if (g_health <= 0) return;
  int facing = g_hero.facing;
  player_init(&g_hero, entry_x, entry_y, g_prog.abilities);
  g_hero.facing = facing;
  g_hero.hurt_timer = 0;
  takes_init(&g_takes, g_prog.take_slots, g_prog.take_frames);
}

static void die(void) {
  g_events |= GE_DEATH;
  g_pending_script = SCRIPT_DEATH0 + (int)(g_prog.play_ticks & 3);
  g_health = g_prog.max_health;
  g_hero.vx = 0;
  g_hero.vy = 0;
  game_enter_room(g_prog.save_room, g_prog.save_x, g_prog.save_y);
}

/* Room edges: walk off a side into the neighbor, fall through the bottom,
 * or jump through the top. Returns 1 if the room changed. */
static int check_edges(int held) {
  struct PakRoom* pr = pak_room(g_room_index);
  int px = g_hero.x >> 8;
  int py = g_hero.y >> 8;
  int mid_y = room_oy + py + (PLAYER_H >> 1);
  int mid_x = room_ox + px + (PLAYER_W >> 1);
  int target = -1;
  int nx = 0;
  int ny = 0;
  if (px >= g_room_pw - PLAYER_W && (held & IN_RIGHT)) {
    target = room_at(pr->region, room_ox + g_room_pw + 1, mid_y);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = 0;
      ny = room_oy + py - (t->wy << 3);
    }
  } else if (px <= 0 && (held & IN_LEFT)) {
    target = room_at(pr->region, room_ox - 2, mid_y);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = (t->w << 3) - PLAYER_W;
      ny = room_oy + py - (t->wy << 3);
    }
  } else if (py >= g_room_ph - PLAYER_H) {
    target = room_at(pr->region, mid_x, room_oy + g_room_ph + 1);
    if (target < 0) {
      hazard_respawn();
      return 0;
    }
    struct PakRoom* t = pak_room(target);
    nx = room_ox + px - (t->wx << 3);
    ny = 0;
  } else if (py <= 0 && g_hero.vy < 0) {
    target = room_at(pr->region, mid_x, room_oy - 2);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = room_ox + px - (t->wx << 3);
      ny = (t->h << 3) - PLAYER_H - 1;
      if (g_hero.vy > -1150) g_hero.vy = -1150; /* carry Ruby up onto the next ledge */
    }
  }
  if (target < 0) return 0;
  struct PakRoom* t = pak_room(target);
  if (nx < 0) nx = 0;
  if (nx > (t->w << 3) - PLAYER_W) nx = (t->w << 3) - PLAYER_W;
  if (ny < 0) ny = 0;
  if (ny > (t->h << 3) - PLAYER_H) ny = (t->h << 3) - PLAYER_H;
  game_enter_room(target, nx, ny);
  return 1;
}

/* ---- enemies ---- */

static void hurt_hero(struct Ent* e) {
  int dir = (e->x + (e->w >> 1)) < (g_hero.x + (PLAYER_W << 7)) ? -1 : 1;
  if (player_hurt(&g_hero, dir)) {
    g_health--;
    g_events |= GE_HURT;
  }
}

static void damage(struct Ent* e, int from_x, int amount) {
  if (e->hurt) return;
  e->hp -= amount;
  e->hurt = HURT_INVULN;
  g_events |= GE_HIT;
  int dir = from_x < e->x + (e->w >> 1) ? 1 : -1;
  if (e->kind != ENT_FLYER) e->vx = dir * 256;
  if (e->hp <= 0) {
    g_events |= GE_KILL;
    int kind = e->kind;
    int x = e->x;
    int y = e->y;
    e->alive = 0;
    e->kind = ENT_NONE;
    if (kind != ENT_SHOT) {
      struct Ent* coin = ent_alloc();
      if (coin) {
        ent_setup(coin, ENT_COIN, 0, 0, 1, 0, 0, -1);
        coin->x = x;
        coin->y = y;
        coin->vy = -512;
        coin->state = 1;   /* a dropped coin falls and settles */
      }
    }
  }
}

static void step_walker(struct Ent* e) {
  int px = e->x >> 8;
  int py = e->y >> 8;
  int w = e->w >> 8;
  int h = e->h >> 8;
  if (e->hurt) {
    e->x += e->vx;
    e->vx -= e->vx >> 3;
    return;
  }
  int ahead = e->dir > 0 ? px + w : px - 1;
  if (solid_px(ahead, py + h - 2) || !floor_px(ahead, py + h + 1)) e->dir = -e->dir;
  e->x += e->dir * WALKER_SPEED;
}

static void step_charger(struct Ent* e) {
  int px = e->x >> 8;
  int py = e->y >> 8;
  int w = e->w >> 8;
  int h = e->h >> 8;
  int hx = g_hero.x >> 8;
  int hy = g_hero.y >> 8;
  if (e->state == 0) {          /* watching */
    int dy = hy - py;
    int dx = hx - px;
    if (dy > -16 && dy < 16 && dx > -120 && dx < 120) {
      e->dir = dx < 0 ? -1 : 1;
      e->state = 1;
      e->timer = 20;            /* wind-up before the rush */
    }
  } else if (e->state == 1) {
    if (--e->timer <= 0) e->state = 2;
  } else if (e->state == 2) {   /* rushing */
    int ahead = e->dir > 0 ? px + w + 1 : px - 2;
    if (solid_px(ahead, py + h - 2) || !floor_px(ahead, py + h + 1)) {
      e->state = 3;
      e->timer = 60;            /* dazed */
    } else {
      e->x += e->dir * CHARGER_SPEED;
    }
  } else {
    if (--e->timer <= 0) e->state = 0;
  }
}

static void step_hopper(struct Ent* e) {
  int px = e->x >> 8;
  int py = e->y >> 8;
  int h = e->h >> 8;
  int w = e->w >> 8;
  int grounded = floor_px(px + 1, py + h) || floor_px(px + w - 2, py + h);
  if (grounded && e->vy >= 0) {
    e->vy = 0;
    e->vx = 0;
    e->y = ((py + h) & ~7) - h;
    e->y <<= 8;
    if (++e->timer >= 70) {
      e->timer = 0;
      e->dir = (g_hero.x < e->x) ? -1 : 1;
      e->vx = e->dir * 200;
      e->vy = -HOPPER_JUMP;
    }
  } else {
    e->vy += ENT_GRAVITY;
    if (e->vy > ENT_FALL_MAX) e->vy = ENT_FALL_MAX;
    int nx = (e->x + e->vx) >> 8;
    if (!solid_px(e->vx > 0 ? nx + w : nx, py + (h >> 1))) e->x += e->vx;
    int ny = (e->y + e->vy) >> 8;
    if (e->vy < 0 && solid_px(px + (w >> 1), ny)) e->vy = 0;
    else e->y += e->vy;
  }
}

static void step_flyer(struct Ent* e) {
  e->anim++;
  int range = (e->a > 0 ? e->a : 6) << 11;   /* tiles -> Q8 pixels */
  int home_x = ((e->tx << 3) << 8);
  if (e->state == 0) {
    e->x += e->dir * 96;
    if (e->x < home_x - range) e->dir = 1;
    if (e->x > home_x + range) e->dir = -1;
    int phase = (e->anim >> 2) & 15;
    int wave = phase < 8 ? phase - 4 : 12 - phase;
    e->y = e->c + (wave << 8);
    int dx = (g_hero.x - e->x) >> 8;
    int dy = (g_hero.y - e->y) >> 8;
    if (dx > -48 && dx < 48 && dy > 0 && dy < 72 && e->timer == 0) {
      e->state = 1;
      e->vx = dx > 0 ? 256 : -256;
      e->vy = 512;
    }
    if (e->timer > 0) e->timer--;
  } else {
    e->x += e->vx;
    e->y += e->vy;
    e->vy -= 16;
    if (e->vy < -256 || e->y <= e->c) {
      e->state = 0;
      e->timer = 90;
      if (e->y < e->c) e->y = e->c;
    }
  }
}

static void step_thrower(struct Ent* e) {
  int dx = (g_hero.x - e->x) >> 8;
  e->dir = dx < 0 ? -1 : 1;
  if (++e->timer >= 100) {
    e->timer = 0;
    if (dx > -170 && dx < 170) {
      struct Ent* s = ent_alloc();
      if (s) {
        ent_setup(s, ENT_SHOT, 0, 0, 0, 0, 0, -1);
        s->x = e->x + (e->dir > 0 ? e->w : -(6 << 8));
        s->y = e->y + (2 << 8);
        int speed = dx < 0 ? -dx : dx;
        speed = (speed << 8) / 60;          /* reach Ruby in about a second */
        if (speed < 160) speed = 160;
        if (speed > 640) speed = 640;
        s->vx = e->dir * speed;
        s->vy = -700;
        s->timer = SHOT_TICKS;
        e->state = 8;                       /* throw pose ticks */
      }
    }
  }
  if (e->state > 0) e->state--;
}

static void step_shot(struct Ent* e) {
  e->vy += 24;
  e->x += e->vx;
  e->y += e->vy;
  if (--e->timer <= 0 || solid_px((e->x >> 8) + 3, (e->y >> 8) + 3)) {
    e->alive = 0;
    e->kind = ENT_NONE;
  }
}

static void step_sandbag(struct Ent* e) {
  if (e->state == 0) {
    int hx = g_hero.x >> 8;
    int ex = e->x >> 8;
    if (hx + PLAYER_W > ex - 4 && hx < ex + 12 && g_hero.y > e->y) e->state = 1;
  } else if (e->state == 1) {
    e->vy += ENT_GRAVITY;
    if (e->vy > ENT_FALL_MAX) e->vy = ENT_FALL_MAX;
    int ny = (e->y + e->vy) >> 8;
    if (floor_px((e->x >> 8) + 4, ny + 8)) {
      e->y = (((ny + 8) & ~7) - 8) << 8;
      e->state = 2;
      e->vy = 0;
    } else {
      e->y += e->vy;
    }
  }
}

static void step_coin(struct Ent* e) {
  e->anim++;
  if (e->state != 1) return;
  e->vy += ENT_GRAVITY;
  if (e->vy > ENT_FALL_MAX) e->vy = ENT_FALL_MAX;
  int ny = (e->y + e->vy) >> 8;
  if (e->vy > 0 && floor_px((e->x >> 8) + 3, ny + 6)) {
    e->y = (((ny + 6) & ~7) - 6) << 8;
    e->vy = 0;
    e->state = 2;
  } else {
    e->y += e->vy;
  }
}

/* ---- Sergeant Gearhart, the Clockwork Constable (boss 1) ----
 *
 * States: 0 intro (waits for the intro card), 1 idle, 2 slam wind-up,
 * 3 slam recovery, 4 charge wind-up, 5 charging, 6 dazed, 7 whistle,
 * 8 defeated. He aims his charge at the nearest of Ruby and her takes, so a
 * take is a decoy. Claps land at any time except mid-charge; while he is
 * dazed (after hitting a wall) each hit counts double. */

#define B_INTRO 0
#define B_IDLE 1
#define B_SLAM_UP 2
#define B_SLAM_DOWN 3
#define B_CHARGE_UP 4
#define B_CHARGE 5
#define B_DAZED 6
#define B_WHISTLE 7
#define B_DOWN 8

static int boss_cycle;

/* x (Q8) of the nearest charge target: Ruby or any active take. */
static int boss_target_x(struct Ent* e) {
  int best = g_hero.x;
  int ex = e->x + (e->w >> 1);
  int bd = best - ex;
  if (bd < 0) bd = -bd;
  for (int s = 0; s < TAKE_MAX_SLOTS; s++) {
    struct Take* t = &g_takes.slots[s];
    if (!t->active || t->length == 0) continue;
    int tx = t->frames[t->cursor].x;
    int d = tx - ex;
    if (d < 0) d = -d;
    if (d < bd) {
      bd = d;
      best = tx;
    }
  }
  return best;
}

static void spawn_wave(int x, int y, int dir) {
  struct Ent* w = ent_alloc();
  if (!w) return;
  ent_setup(w, ENT_WAVE, 0, 0, 0, 0, 0, -1);
  w->x = x;
  w->y = y;
  w->vx = dir * WAVE_SPEED;
  w->timer = 240;
}

static void spawn_pigeon(int x, int y) {
  struct Ent* f = ent_alloc();
  if (!f) return;
  ent_setup(f, ENT_FLYER, x >> 11, y >> 11, 5, 0, 0, -1);
}

static void step_sergeant(struct Ent* e) {
  int floor_y = e->y + e->h;
  switch (e->state) {
    case B_INTRO:
      g_pending_script = SCRIPT_S1_BOSS_INTRO;
      e->state = B_IDLE;
      e->timer = 60;
      break;
    case B_IDLE:
      e->dir = (g_hero.x < e->x) ? -1 : 1;
      if (--e->timer <= 0) {
        int pick = boss_cycle % 3;
        if (e->hp <= BOSS1_HP / 2 && pick == 2) pick = 1;   /* angrier: fewer whistles */
        boss_cycle++;
        if (pick == 0) {
          e->state = B_SLAM_UP;
          e->timer = 36;
        } else if (pick == 1) {
          e->state = B_CHARGE_UP;
          e->timer = 40;
          e->dir = boss_target_x(e) < e->x ? -1 : 1;
        } else {
          e->state = B_WHISTLE;
          e->timer = 50;
        }
      }
      break;
    case B_SLAM_UP:
      if (--e->timer <= 0) {
        spawn_wave(e->x - (8 << 8), floor_y - (8 << 8), -1);
        spawn_wave(e->x + e->w, floor_y - (8 << 8), 1);
        g_events |= GE_SLAM;
        e->state = B_SLAM_DOWN;
        e->timer = 30;
      }
      break;
    case B_SLAM_DOWN:
      if (--e->timer <= 0) {
        e->state = B_IDLE;
        e->timer = e->hp <= BOSS1_HP / 2 ? 30 : 50;
      }
      break;
    case B_CHARGE_UP:
      if (--e->timer <= 0) e->state = B_CHARGE;
      break;
    case B_CHARGE: {
      int speed = e->hp <= BOSS1_HP / 2 ? 960 : 800;
      int px = e->x >> 8;
      int py = e->y >> 8;
      int w = e->w >> 8;
      int h = e->h >> 8;
      int ahead = e->dir > 0 ? px + w + 2 : px - 3;
      if (solid_px(ahead, py + h - 4) || solid_px(ahead, py + 4)) {
        e->state = B_DAZED;
        e->timer = 110;
        g_events |= GE_SLAM;
      } else {
        e->x += e->dir * speed;
      }
      break;
    }
    case B_DAZED:
      if (--e->timer <= 0) {
        e->state = B_IDLE;
        e->timer = 40;
      }
      break;
    case B_WHISTLE:
      if (e->timer == 30) {
        spawn_pigeon(4 << 11, 8 << 11);
        spawn_pigeon(34 << 11, 8 << 11);
      }
      if (--e->timer <= 0) {
        e->state = B_IDLE;
        e->timer = 40;
      }
      break;
    default:
      break;
  }
}

/* ---- the Iron Horse (boss 2) ----
 *
 * A locomotive that runs the length of the arena from alternating sides,
 * spitting coals. Holding the brake (switch 3: a mark Ruby or a take stands
 * on) while it is on screen stops it; stopped, its firebox is exposed and
 * claps land. States: 0 intro, 10 waiting off-screen, 11 running, 12
 * braking, 13 stopped, 14 leaving. */

#define H_WAIT 10
#define H_RUN 11
#define H_BRAKE 12
#define H_STOPPED 13
#define H_LEAVE 14

static void spawn_coal(struct Ent* e) {
  struct Ent* c = ent_alloc();
  if (!c) return;
  ent_setup(c, ENT_SHOT, 0, 0, 0, 1, 0, -1);   /* b = 1: drawn as a coal */
  c->x = e->x + (e->dir < 0 ? (18 << 8) : (46 << 8));
  c->y = e->y - (6 << 8);
  c->vx = -e->dir * 160;
  c->vy = -820;
  c->timer = SHOT_TICKS;
}

static void step_horse(struct Ent* e) {
  int pw = g_room_pw << 8;
  int on_screen = e->x + e->w > 0 && e->x < pw;
  switch (e->state) {
    case B_INTRO:
      g_pending_script = SCRIPT_S2_BOSS_INTRO;
      e->state = H_WAIT;
      e->timer = 60;
      e->dir = -1;
      e->x = pw + (24 << 8);
      break;
    case H_WAIT:
      if (--e->timer <= 0) {
        e->state = H_RUN;
        e->vx = e->dir * (e->hp <= HORSE_HP / 2 ? 1024 : 800);
        e->x = e->dir < 0 ? pw + (8 << 8) : -e->w - (8 << 8);
        e->timer = 0;
      }
      break;
    case H_RUN:
      e->x += e->vx;
      if ((++e->timer % 50) == 25 && on_screen) spawn_coal(e);
      if (g_switch[3] && on_screen && e->x > (16 << 8) && e->x + e->w < pw - (16 << 8)) {
        e->state = H_BRAKE;
        g_events |= GE_SLAM;
      }
      if ((e->dir < 0 && e->x + e->w < -(16 << 8)) || (e->dir > 0 && e->x > pw + (16 << 8))) {
        e->state = H_WAIT;
        e->timer = 50;
        e->dir = -e->dir;
      }
      break;
    case H_BRAKE:
      e->x += e->vx;
      e->vx -= e->vx >> 3;
      if (e->vx > -40 && e->vx < 40) {
        e->vx = 0;
        e->state = H_STOPPED;
        e->timer = 150;
      }
      break;
    case H_STOPPED:
      if (--e->timer <= 0) {
        e->state = H_LEAVE;
        e->vx = 0;
      }
      break;
    case H_LEAVE:
      e->vx += e->dir * 24;
      e->x += e->vx;
      if ((e->dir < 0 && e->x + e->w < -(16 << 8)) || (e->dir > 0 && e->x > pw + (16 << 8))) {
        e->state = H_WAIT;
        e->timer = 60;
        e->dir = -e->dir;
      }
      break;
    default:
      break;
  }
}

static void step_boss(struct Ent* e) {
  if (e->a == 2) step_horse(e);
  else step_sergeant(e);
}

/* Whether claps land on the boss now, and how hard. */
static int boss_hit_value(struct Ent* e) {
  if (e->a == 2) return e->state == H_STOPPED ? 1 : 0;
  if (e->state == B_CHARGE || e->state == B_INTRO) return 0;
  return e->state == B_DAZED ? 2 : 1;
}

/* Whether touching the boss hurts Ruby. */
static int boss_contact_hurts(struct Ent* e) {
  if (e->a == 2) return e->state == H_RUN || e->state == H_BRAKE || e->state == H_LEAVE;
  return e->state != B_DAZED;
}

static void boss_defeated(struct Ent* e) {
  e->state = B_DOWN;
  e->alive = 0;
  e->kind = ENT_NONE;
  flag_set(BOSS_FLAG_BASE + e->a);
  g_events |= GE_BOSS_DOWN;
  g_boss = 0;
  /* the wire falls where he stood; the way home opens */
  int ability = e->a == 2 ? AB_JUMPCUT : AB_WIRE;
  int script = e->a == 2 ? SCRIPT_S2_WRAP : SCRIPT_S1_WRAP;
  int home_tx = e->a == 2 ? 33 : 15;
  int prize_tx = (e->x >> 11) + 1;
  if (prize_tx < 3) prize_tx = 3;
  if (prize_tx > g_room.w - 4) prize_tx = g_room.w - 4;
  struct Ent* prize = ent_alloc();
  if (prize) {
    ent_setup(prize, ENT_ABILITY, prize_tx, e->ty, ability, script, 0, -1);
  }
  struct Ent* door = ent_alloc();
  if (door) {
    ent_setup(door, ENT_DOOR, 4, e->ty, ROOM_BACKLOT_WEST, home_tx, 25, -1);
  }
  for (int i = 0; i < g_ent_count; i++) {
    if (g_ents[i].kind == ENT_FLYER || g_ents[i].kind == ENT_WAVE || g_ents[i].kind == ENT_SHOT) {
      g_ents[i].alive = 0;
      g_ents[i].kind = ENT_NONE;
    }
  }
}

static void step_wave(struct Ent* e) {
  e->x += e->vx;
  int px = (e->x >> 8) + (e->vx > 0 ? 8 : -1);
  if (--e->timer <= 0 || solid_px(px, (e->y >> 8) + 4)) {
    e->alive = 0;
    e->kind = ENT_NONE;
  }
}

/* ---- moving platforms ----
 * Advanced before player_step so the controller sees this tick's position
 * and displacement (struct Platform contract in player.h): ping-pong along
 * one axis over `a` tiles at `c` Q8 pixels per tick. */

static int mover_dx[8];
static int mover_dy[8];

static void step_movers(void) {
  int k = 0;
  for (int i = 0; i < g_ent_count && k < MAX_MOVERS; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive || e->kind != ENT_PLATFORM) continue;
    int range = (e->a > 0 ? e->a : 4) << 11;
    int step = e->dir * e->c;
    int next = e->state + step;
    if (next < 0) {
      next = 0;
      e->dir = 1;
    }
    if (next > range) {
      next = range;
      e->dir = -1;
    }
    int delta = next - e->state;
    e->state = next;
    mover_dx[k] = 0;
    mover_dy[k] = 0;
    if (e->b) {
      e->y += delta;
      mover_dy[k] = delta;
    } else {
      e->x += delta;
      mover_dx[k] = delta;
    }
    k++;
  }
}

/* Append moving platforms after the takes' entries; returns the total. */
static int fill_platforms(int count) {
  int k = 0;
  for (int i = 0; i < g_ent_count && k < MAX_MOVERS; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive || e->kind != ENT_PLATFORM) continue;
    struct Platform* p = &g_platforms[count];
    p->x = e->x;
    p->y = e->y;
    p->w = e->w;
    p->h = e->h;
    p->dx = mover_dx[k];
    p->dy = mover_dy[k];
    p->id = 8 + k;
    p->active = 1;
    count++;
    k++;
  }
  return count;
}

/* ---- per-tick entity work ---- */

static void update_switches(void) {
  for (int i = 0; i < MAX_SWITCHES; i++) g_switch[i] = 0;
  struct Hitbox hb;
  hero_box(&hb);
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive || e->kind != ENT_PLATE) continue;
    struct Hitbox pb;
    ent_box(e, &pb);
    int pressed = hitbox_overlap(&hb, &pb);
    for (int s = 0; s < TAKE_MAX_SLOTS && !pressed; s++) {
      struct Take* t = &g_takes.slots[s];
      if (!t->active || t->length == 0) continue;
      struct TakeFrame* f = &t->frames[t->cursor];
      struct Hitbox tb;
      tb.x = f->x;
      tb.y = f->y;
      tb.w = PLAYER_W * Q8;
      tb.h = PLAYER_H * Q8;
      if (hitbox_overlap(&tb, &pb)) pressed = 1;
    }
    e->state = pressed;
    if (pressed) g_switch[e->a & 15] = 1;
  }
}

static void combat(void) {
  struct Hitbox live;
  int has_live = player_attack_box(&g_hero, &live);
  struct Hitbox film[3];
  int has_film[3];
  for (int s = 0; s < TAKE_MAX_SLOTS; s++) {
    has_film[s] = g_takes.slots[s].active && take_attack_box(&g_takes.slots[s], &film[s]);
  }
  struct Hitbox hb;
  hero_box(&hb);
  int pogo = 0;
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive) continue;
    if (e->kind == ENT_WAVE) {
      struct Hitbox wb;
      ent_box(e, &wb);
      if (hitbox_overlap(&hb, &wb)) hurt_hero(e);
      continue;
    }
    if (e->kind == ENT_BOSS) {
      struct Hitbox bb;
      ent_box(e, &bb);
      int amount = boss_hit_value(e);
      int hit = 0;
      if (amount > 0 && !e->hurt) {
        if (has_live && hitbox_overlap(&live, &bb)) {
          hit = 1;
          if (g_hero.attack_dir == 3) pogo = 1;
        }
        for (int s = 0; s < TAKE_MAX_SLOTS && !hit; s++) {
          if (has_film[s] && hitbox_overlap(&film[s], &bb)) hit = 1;
        }
      }
      if (hit) {
        e->hp -= amount;
        e->hurt = 16;
        g_events |= GE_HIT | GE_BOSS_HIT;
        if (e->hp <= 0) {
          boss_defeated(e);
          continue;
        }
      }
      if (hitbox_overlap(&hb, &bb) && boss_contact_hurts(e)) hurt_hero(e);
      continue;
    }
    int enemy = is_enemy(e->kind);
    if (!enemy && e->kind != ENT_SHOT) continue;
    struct Hitbox eb;
    ent_box(e, &eb);
    if (has_live && hitbox_overlap(&live, &eb)) {
      if (e->kind == ENT_SHOT) {
        e->alive = 0;
        e->kind = ENT_NONE;
        g_events |= GE_HIT;
      } else {
        damage(e, g_hero.x, 1);
      }
      if (g_hero.attack_dir == 3) pogo = 1;
      continue;
    }
    for (int s = 0; s < TAKE_MAX_SLOTS; s++) {
      if (has_film[s] && hitbox_overlap(&film[s], &eb) && enemy) {
        damage(e, film[s].x, 1);
        break;
      }
    }
    if (!e->alive) continue;
    if (hitbox_overlap(&hb, &eb)) {
      if (e->kind == ENT_SHOT) {
        hurt_hero(e);
        e->alive = 0;
        e->kind = ENT_NONE;
      } else if (!(e->kind == ENT_CHARGER && e->state == 3)) {
        hurt_hero(e);
      }
    }
  }
  if (pogo) {
    g_hero.vy = -POGO_SPEED;
    g_hero.air_jump_left = 1;
    g_hero.on_ground = 0;
  }
}

static void pickups_and_interactions(int pressed) {
  struct Hitbox hb;
  hero_box(&hb);
  int want = (pressed & (IN_INTERACT | IN_UP)) != 0;
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive) continue;
    struct Hitbox eb;
    ent_box(e, &eb);
    if (!hitbox_overlap(&hb, &eb)) continue;
    switch (e->kind) {
      case ENT_COIN:
        if (e->state == 1) break;
        g_prog.scrip += e->a > 0 ? e->a : 1;
        if (g_prog.scrip > 9999) g_prog.scrip = 9999;
        flag_set(e->flag);
        e->alive = 0;
        e->kind = ENT_NONE;
        g_events |= GE_COIN;
        break;
      case ENT_HEART:
        if (g_health < g_prog.max_health) {
          g_health++;
          e->alive = 0;
          g_events |= GE_PICKUP;
        }
        break;
      case ENT_FRAME:
        g_prog.max_health++;
        g_health = g_prog.max_health;
        flag_set(e->flag);
        e->alive = 0;
        g_events |= GE_PICKUP;
        break;
      case ENT_REEL:
        flag_set(e->flag);
        e->alive = 0;
        g_events |= GE_PICKUP;
        g_pending_script = e->a;
        break;
      case ENT_ABILITY:
        g_prog.abilities |= e->a;
        g_hero.abilities = g_prog.abilities;
        flag_set(e->flag);
        e->alive = 0;
        g_events |= GE_PICKUP;
        g_pending_script = e->b;
        break;
      case ENT_SPRING:
        if (g_hero.vy > 0) {
          g_hero.vy = -SPRING_SPEED;
          g_hero.air_jump_left = 1;
          g_hero.on_ground = 0;
          e->timer = 12;
          g_events |= GE_SPRING;
        }
        break;
      case ENT_SANDBAG:
        if (e->state == 1) hurt_hero(e);
        break;
      case ENT_MIRROR:
        if (want && g_hero.on_ground) {
          g_prog.save_room = g_room_index;
          g_prog.save_x = g_hero.x >> 8;
          g_prog.save_y = g_hero.y >> 8;
          g_health = g_prog.max_health;
          game_save();
          g_events |= GE_SAVE;
          e->timer = 60;
        }
        break;
      case ENT_SIGN:
      case ENT_NPC:
        if (want && g_hero.on_ground) g_pending_script = e->a;
        break;
      case ENT_SHOP:
        if (want && g_hero.on_ground) g_pending_shop = 1;
        break;
      case ENT_DOOR:
        if (want && g_hero.on_ground) {
          /* d bit 0: drawn by the room art; d bits 4..7: boss that must be
           * defeated first (a sealed stage door until then) */
          int needs = (e->d >> 4) & 15;
          if (needs && !flag_get(BOSS_FLAG_BASE + needs)) {
            g_pending_script = SCRIPT_STAGE_SEALED;
            break;
          }
          game_enter_room(e->a, e->b << 3, (e->c << 3) + TILE - PLAYER_H);
          return;
        }
        break;
      default:
        break;
    }
  }
}

static void step_entities(void) {
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive) continue;
    if (e->hurt) e->hurt--;
    switch (e->kind) {
      case ENT_WALKER: step_walker(e); break;
      case ENT_CHARGER: step_charger(e); break;
      case ENT_HOPPER: step_hopper(e); break;
      case ENT_FLYER: step_flyer(e); break;
      case ENT_THROWER: step_thrower(e); break;
      case ENT_SHOT: step_shot(e); break;
      case ENT_SANDBAG: step_sandbag(e); break;
      case ENT_COIN: step_coin(e); break;
      case ENT_BOSS: step_boss(e); break;
      case ENT_WAVE: step_wave(e); break;
      default:
        if (e->timer > 0 && e->kind != ENT_GATE) e->timer--;
        e->anim++;
        break;
    }
  }
}

/* ---- the tick ---- */

void game_tick(int held, int pressed) {
  g_events = 0;
  g_prog.play_ticks++;
  takes_advance(&g_takes);
  step_movers();
  g_room.platform_count = fill_platforms(takes_platforms(&g_takes, g_platforms));
  g_room.platforms = g_platforms;
  player_step(&g_hero, &g_room, held, pressed);
  int rec = takes_capture(&g_takes, &g_hero, (pressed & IN_RECORD) != 0);
  if (rec == 1) g_events |= GE_REC_START;
  if (rec == 2) g_events |= GE_REC_STOP;
  if ((pressed & IN_JUMPCUT) && (g_hero.abilities & AB_JUMPCUT)) {
    int limit = g_takes.slot_limit;
    int slot = g_takes.replace_next;
    for (int i = 0; i < limit; i++) {
      slot = slot - 1;
      if (slot < 0) slot = limit - 1;
      if (takes_jumpcut(&g_takes, &g_hero, &g_room, slot)) {
        g_events |= GE_JUMPCUT;
        break;
      }
    }
  }
  step_entities();
  update_switches();
  update_gates();
  combat();
  pickups_and_interactions(pressed);
  if (g_events & GE_ROOM) return;   /* a door moved us; the new room starts clean */
  if (g_hero.events & EV_HAZARD) hazard_respawn();
  if (g_health <= 0) {
    die();
    return;
  }
  check_edges(held);
  if (g_health <= 0) die();   /* a pit at a room's bottom edge */
}

/* ---- new game, save and load ---- */

void game_new(int start_room) {
  for (int i = 0; i < 64; i++) g_prog.flags[i] = 0;
  for (int i = 0; i < 8; i++) g_prog.visited[i] = 0;
  g_prog.max_health = 5;
  g_prog.scrip = 0;
  g_prog.abilities = 0;
  g_prog.take_slots = 1;
  g_prog.take_frames = TAKE_BASE_FRAMES;
  g_prog.play_ticks = 0;
  struct PakRoom* pr = pak_room(start_room);
  g_prog.save_room = start_room;
  g_prog.save_x = pr->spawn_x << 3;
  g_prog.save_y = (pr->spawn_y << 3) + TILE - PLAYER_H;
  g_health = g_prog.max_health;
  g_hero.vx = 0;
  g_hero.vy = 0;
  g_hero.facing = 1;
  game_enter_room(start_room, g_prog.save_x, g_prog.save_y);
}

/* The save file is a magic word, the pack size (so a save from a different
 * world layout is ignored rather than misread) and the Progress record. */
int game_save(void) {
  int fd = open(SAVE_PATH);
  if (fd < 0) return -1;
  unsigned head[2];
  head[0] = SAVE_MAGIC;
  head[1] = PAK_SIZE;
  int ok = write(fd, head, 8) == 8;
  ok = ok && write(fd, &g_prog, sizeof(struct Progress)) == (int)sizeof(struct Progress);
  close(fd);
  return ok ? 0 : -1;
}

int game_load(void) {
  int fd = open_existing(SAVE_PATH);
  if (fd < 0) return -1;
  unsigned head[2];
  int ok = read(fd, head, 8) == 8 && head[0] == SAVE_MAGIC && head[1] == PAK_SIZE;
  ok = ok && read(fd, &g_prog, sizeof(struct Progress)) == (int)sizeof(struct Progress);
  close(fd);
  if (!ok) return -1;
  if (g_prog.save_room < 0 || g_prog.save_room >= pak_room_count()) return -1;
  g_health = g_prog.max_health;
  game_enter_room(g_prog.save_room, g_prog.save_x, g_prog.save_y);
  return 0;
}
