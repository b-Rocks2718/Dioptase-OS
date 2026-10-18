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
#define KRAKEN_HP 12
#define CAMERA_HP 18
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
static int boss_cycle;         /* restart each encounter, including after death */

/* ---- persistent flags ---- */

/* Invalid or absent persistence bits are false, including transient flag -1. */
int flag_get(int id) {
  if (id < 0 || id >= MAX_FLAGS) return 0;
  return (g_prog.flags[id >> 5] >> (id & 31)) & 1;
}

/* Only bounded persistent flag ids may modify campaign progress. */
void flag_set(int id) {
  if (id < 0 || id >= MAX_FLAGS) return;
  g_prog.flags[id >> 5] |= 1u << (id & 31);
}

/* Mark a room discovered for the campaign map (the pack supports 256 rooms). */
static void mark_visited(int room) {
  g_prog.visited[(room >> 5) & 7] |= 1u << (room & 31);
}

/* ---- tile queries for entities (pixel coordinates) ---- */

/* Enemy tile queries treat the outside of the current room as solid. */
static int col_px(int x, int y) {
  if (x < 0 || y < 0 || x >= g_room_pw || y >= g_room_ph) return COL_SOLID;
  return g_room.col[g_room.row[y >> 3] + (x >> 3)];
}

/* Only full solids block horizontal or upward enemy motion. */
static int solid_px(int x, int y) {
  return col_px(x, y) == COL_SOLID;
}

/* Enemy feet can stand on solid tiles and one-way ledges. */
static int floor_px(int x, int y) {
  int c = col_px(x, y);
  return c == COL_SOLID || c == COL_ONEWAY;
}

/* Expose an entity body in the same Q8 space used by clapper contacts. */
static void ent_box(struct Ent* e, struct Hitbox* b) {
  b->x = e->x;
  b->y = e->y;
  b->w = e->w;
  b->h = e->h;
}

/* Expose Ruby's physical body, excluding her clapper reach. */
static void hero_box(struct Hitbox* b) {
  b->x = g_hero.x;
  b->y = g_hero.y;
  b->w = PLAYER_W * Q8;
  b->h = PLAYER_H * Q8;
}

/* ---- entities ---- */

/* Identify damageable ordinary enemies, excluding bosses and projectiles. */
static int is_enemy(int kind) {
  return kind == ENT_WALKER || kind == ENT_HOPPER || kind == ENT_FLYER || kind == ENT_THROWER ||
         kind == ENT_CHARGER;
}

/* Reuse a dead slot or append within the fixed entity budget; return zero if full. */
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
    case ENT_TENTACLE: w = 10; h = 56; break;
    case ENT_BOSS:
      if (a == 4) {
        w = 44;
        h = 40;
        e->hp = CAMERA_HP;
      } else if (a == 3) {
        w = 8;
        h = 8;
        e->hp = KRAKEN_HP;
      } else if (a == 2) {
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
  e->x = (tx * TILE + 4 - (w >> 1)) * Q8;
  e->y = (ty * TILE + 8 - h) * Q8;
  if (kind == ENT_GATE) {
    e->x = tx * TILE * Q8;
    e->y = ty * TILE * Q8;
  }
  if (kind == ENT_SANDBAG) e->y = ty * TILE * Q8;
  if (kind == ENT_FLYER) {
    e->c = e->y;   /* home height */
  }
}

/* Reconstruct persistent boss rewards on both victory and later room visits. */
static void boss_rewards(int boss, int tx) {
  /* each stage's prize, its wrap card, and the way home to its stage door */
  int ability = AB_WIRE;
  int script = SCRIPT_S1_WRAP;
  int home_tx = 15;
  int ty = 25;
  if (boss == 2) {
    ability = AB_JUMPCUT;
    script = SCRIPT_S2_WRAP;
    home_tx = 33;
  } else if (boss == 3) {
    ability = AB_GLIDE;
    script = SCRIPT_S3_WRAP;
    home_tx = 51;
    ty = 23;
  } else if (boss != 1) {
    return;
  }
  if (tx < 3) tx = 3;
  if (tx > g_room.w - 4) tx = g_room.w - 4;
  if (!(g_prog.abilities & ability)) {
    struct Ent* prize = ent_alloc();
    if (prize) ent_setup(prize, ENT_ABILITY, tx, ty, ability, script, 0, -1);
  }
  struct Ent* door = ent_alloc();
  if (door) ent_setup(door, ENT_DOOR, 4, ty, ROOM_BACKLOT_WEST, home_tx, 25, -1);
}

/* Materialize the room's entities, then reconstruct defeated-boss rewards. */
static void spawn_room_entities(void) {
  struct PakRoom* pr = pak_room(g_room_index);
  short* raw = (short*)(g_pak + pr->entities_off);
  g_ent_count = 0;
  g_boss = 0;
  boss_cycle = 0;
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
  for (int i = 0; i < pr->entity_count; i++) {
    short* r = raw + (i << 3);
    if (r[0] == ENT_BOSS && flag_get(BOSS_FLAG_BASE + r[3])) boss_rewards(r[3], r[1]);
  }
}

/* Apply gates to the room's collision: a closed gate's column is solid. */
static void update_gates(void) {
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive || e->kind != ENT_GATE || e->tx < 0 || e->tx >= g_room.w) continue;
    int open = g_switch[e->a & 15];
    /* A released plate may close behind Ruby, but never through her. Takes
     * are projections, so their overlap does not prevent a gate closing. */
    struct Hitbox hb;
    struct Hitbox gb;
    hero_box(&hb);
    ent_box(e, &gb);
    if (!open && hitbox_overlap(&hb, &gb)) open = 1;
    int want = open ? 1 : 0;
    if (want != e->state || e->timer == 0) {
      int h = e->b > 0 ? e->b : 1;
      for (int k = 0; k < h; k++) {
        int ty = e->ty + k;
        if (ty >= 0 && ty < g_room.h) {
          int cell = g_room.row[ty] + e->tx;
          g_room.col[cell] = open ? g_pak[pak_room(g_room_index)->col_off + cell] : COL_SOLID;
        }
      }
      if (e->timer != 0) g_events |= GE_GATE;
      e->state = want;
      e->timer = 1;
    }
  }
}

/* ---- room changes ---- */

/* Reset room-local state and load collision while retaining the entrance impulse. */
void game_enter_room(int room, int x_px, int y_px) {
  room_load(room);
  struct PakRoom* pr = pak_room(room);
  room_ox = pr->wx * TILE;
  room_oy = pr->wy * TILE;
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
    int x0 = r->wx * TILE;
    int y0 = r->wy * TILE;
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

/* Restore the last mirror checkpoint with full health and an empty take bank. */
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
static int check_edges(void) {
  struct PakRoom* pr = pak_room(g_room_index);
  int px = g_hero.x >> 8;
  int py = g_hero.y >> 8;
  int mid_y = room_oy + py + (PLAYER_H >> 1);
  int mid_x = room_ox + px + (PLAYER_W >> 1);
  int target = -1;
  int nx = 0;
  int ny = 0;
  if (g_hero.edge_vx > 0) {
    target = room_at(pr->region, room_ox + g_room_pw + 1, mid_y);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = 0;
      ny = room_oy + py - (t->wy * TILE);
    }
  } else if (g_hero.edge_vx < 0) {
    target = room_at(pr->region, room_ox - 2, mid_y);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = (t->w << 3) - PLAYER_W;
      ny = room_oy + py - (t->wy * TILE);
    }
  } else if (g_hero.edge_vy > 0) {
    target = room_at(pr->region, mid_x, room_oy + g_room_ph + 1);
    if (target < 0) {
      hazard_respawn();
      return 0;
    }
    struct PakRoom* t = pak_room(target);
    nx = room_ox + px - (t->wx * TILE);
    ny = 0;
  } else if (g_hero.edge_vy < 0) {
    target = room_at(pr->region, mid_x, room_oy - 2);
    if (target >= 0) {
      struct PakRoom* t = pak_room(target);
      nx = room_ox + px - (t->wx * TILE);
      ny = (t->h << 3) - PLAYER_H - 1;
    }
  }
  if (target < 0) return 0;
  struct PakRoom* t = pak_room(target);
  if (nx < 0) nx = 0;
  if (nx > (t->w << 3) - PLAYER_W) nx = (t->w << 3) - PLAYER_W;
  if (ny < 0) ny = 0;
  if (ny > (t->h << 3) - PLAYER_H) ny = (t->h << 3) - PLAYER_H;
  /* A neighbor's rectangle alone is not an opening. Refuse transitions
   * whose whole body would overlap scenery, spikes, or a closed gate. */
  struct Room destination;
  int rows[60];
  destination.w = t->w;
  destination.h = t->h;
  destination.col = g_pak + t->col_off;
  destination.row = rows;
  destination.platform_count = 0;
  destination.platforms = 0;
  for (int y = 0; y < t->h; y++) rows[y] = y * t->w;
  if (!player_can_place(&destination, nx * Q8, ny * Q8)) return 0;
  short* raw = (short*)(g_pak + t->entities_off);
  for (int i = 0; i < t->entity_count; i++) {
    short* e = raw + (i << 3);
    if (e[0] != ENT_GATE) continue;
    int height = e[4] > 0 ? e[4] : 1;
    if (nx < (e[1] + 1) * TILE && nx + PLAYER_W > e[1] * TILE &&
        ny < (e[2] + height) * TILE && ny + PLAYER_H > e[2] * TILE) return 0;
  }
  if (g_hero.edge_vx) g_hero.vx = g_hero.edge_vx;
  if (g_hero.edge_vy) g_hero.vy = g_hero.edge_vy;
  /* The vertical seam hides one body height. A small upward handoff keeps
   * Ruby rising far enough to reach the first ledge in the next screen. */
  if (g_hero.edge_vy < 0 && g_hero.vy > -1150) g_hero.vy = -1150;
  game_enter_room(target, nx, ny);
  return 1;
}

/* ---- enemies ---- */

/* Apply at most one health loss during the controller's invulnerability window. */
static void hurt_hero(struct Ent* e) {
  int dir = (e->x + (e->w >> 1)) < (g_hero.x + (PLAYER_W << 7)) ? -1 : 1;
  if (player_hurt(&g_hero, dir)) {
    g_health--;
    g_events |= GE_HURT;
  }
}

/* Damage an ordinary enemy and replace a defeated body with falling scrip. */
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

/* Patrol a ledge, turning at walls or missing support; recoil briefly interrupts patrol. */
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

/* Telegraph a level rush, then become vulnerable while recovering at an obstruction. */
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

/* Wait on a ledge before hopping toward Ruby with capped falling speed. */
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
    e->y *= Q8;
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

/* Patrol around a home height, with a brief swoop when Ruby passes below. */
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
    e->y = e->c + (wave * Q8);
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

/* Periodically aim a bounded arcing shot toward Ruby's current horizontal position. */
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

/* Expire projectiles on terrain contact, lifetime exhaustion, or leaving the room. */
static void step_shot(struct Ent* e) {
  if (e->b != 2) e->vy += 24;   /* b 2: a film frame flies straight */
  e->x += e->vx;
  e->y += e->vy;
  if (--e->timer <= 0 || solid_px((e->x >> 8) + 3, (e->y >> 8) + 3)) {
    e->alive = 0;
    e->kind = ENT_NONE;
  }
}

/* Drop when Ruby passes below and stop at the first supporting floor. */
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
      e->y = (((ny + 8) & ~7) - 8) * Q8;
      e->state = 2;
      e->vy = 0;
    } else {
      e->y += e->vy;
    }
  }
}

/* Let dropped scrip settle before it becomes collectable. */
static void step_coin(struct Ent* e) {
  e->anim++;
  if (e->state != 1) return;
  e->vy += ENT_GRAVITY;
  if (e->vy > ENT_FALL_MAX) e->vy = ENT_FALL_MAX;
  int ny = (e->y + e->vy) >> 8;
  if (e->vy > 0 && floor_px((e->x >> 8) + 3, ny + 6)) {
    e->y = (((ny + 6) & ~7) - 6) * Q8;
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

/* Allocate a finite-lived ground shockwave, dropping it if the room is full. */
static void spawn_wave(int x, int y, int dir) {
  struct Ent* w = ent_alloc();
  if (!w) return;
  ent_setup(w, ENT_WAVE, 0, 0, 0, 0, 0, -1);
  w->x = x;
  w->y = y;
  w->vx = dir * WAVE_SPEED;
  w->timer = 240;
}

/* Allocate a boss minion at a Q8 position if a room slot remains. */
static void spawn_pigeon(int x, int y) {
  struct Ent* f = ent_alloc();
  if (!f) return;
  ent_setup(f, ENT_FLYER, x >> 11, y >> 11, 5, 0, 0, -1);
}

/* Advance Gearhart's telegraphed slam, charge and whistle encounter states. */
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

/* ---- the Kraken (boss 3) ----
 *
 * The boss entity is an unseen puppeteer under the deck (states 0 intro, 20
 * choosing, 21 waiting). It sends up tentacles (ENT_TENTACLE) near Ruby, two
 * at once below half health. A tentacle bubbles (state 0, a warning), bursts
 * up through the deck (1) and hovers (2), then slams across the deck toward
 * the side Ruby was on (3); it lies there stuck (4), harmless and open to
 * claps, before sinking back (5). The deck surface is the puppeteer's spawn
 * tile's bottom edge. */

#define K_CHOOSE 20
#define K_WAIT 21
#define T_WARN 0
#define T_RISE 1
#define T_HOVER 2
#define T_SLAM 3
#define T_STUCK 4
#define T_SINK 5

static void spawn_tentacle(struct Ent* boss, int tile_x) {
  struct Ent* t = ent_alloc();
  if (!t) return;
  ent_setup(t, ENT_TENTACLE, tile_x, boss->ty, 0, 0, 0, -1);
  int deck = ((boss->ty + 1) << 3) << 8;
  t->x = ((tile_x << 3) + 4 - 5) << 8;
  t->y = deck - (56 << 8);
  t->w = 10 << 8;
  t->h = 56 << 8;
  t->c = deck;                 /* remembered deck surface (Q8) */
  t->b = (tile_x << 3) + 4;    /* center x in pixels */
  t->state = T_WARN;
  t->timer = 50;
}

static int tentacles_alive(void) {
  int n = 0;
  for (int i = 0; i < g_ent_count; i++) {
    if (g_ents[i].alive && g_ents[i].kind == ENT_TENTACLE) n++;
  }
  return n;
}

static void step_kraken(struct Ent* e) {
  switch (e->state) {
    case B_INTRO:
      g_pending_script = SCRIPT_S3_BOSS_INTRO;
      e->state = K_CHOOSE;
      e->timer = 60;
      break;
    case K_CHOOSE:
      if (--e->timer <= 0) {
        int hx = ((g_hero.x >> 8) + 4) >> 3;
        if (hx < 7) hx = 7;
        if (hx > g_room.w - 8) hx = g_room.w - 8;
        spawn_tentacle(e, hx);
        if (e->hp <= KRAKEN_HP / 2) {
          int other = hx < (g_room.w >> 1) ? hx + 14 : hx - 14;
          spawn_tentacle(e, other);
        }
        e->state = K_WAIT;
      }
      break;
    case K_WAIT:
      if (tentacles_alive() == 0) {
        e->state = K_CHOOSE;
        e->timer = e->hp <= KRAKEN_HP / 2 ? 25 : 45;
      }
      break;
    default:
      break;
  }
}

static void step_tentacle(struct Ent* t) {
  int deck = t->c;
  int cx = t->b;
  switch (t->state) {
    case T_WARN:
      if (--t->timer <= 0) {
        t->state = T_RISE;
        t->timer = 16;
      }
      break;
    case T_RISE:
      if (--t->timer <= 0) {
        t->state = T_HOVER;
        t->timer = 22;
      }
      break;
    case T_HOVER:
      t->dir = (g_hero.x >> 8) + 4 < cx ? -1 : 1;
      if (--t->timer <= 0) {
        t->state = T_SLAM;
        t->timer = 6;
        /* lie across the deck toward Ruby */
        t->w = 56 << 8;
        t->h = 10 << 8;
        t->y = deck - (10 << 8);
        t->x = t->dir < 0 ? ((cx - 56) << 8) : (cx << 8);
        g_events |= GE_SLAM;
      }
      break;
    case T_SLAM:
      if (--t->timer <= 0) {
        t->state = T_STUCK;
        t->timer = 100;
      }
      break;
    case T_STUCK:
      if (--t->timer <= 0) {
        t->state = T_SINK;
        t->timer = 12;
      }
      break;
    default:
      if (--t->timer <= 0) {
        t->alive = 0;
        t->kind = ENT_NONE;
      }
      break;
  }
}

/* ---- the Everlasting Camera (boss 4, the finale) ----
 *
 * States: 0 intro, 30 lens shut (fires fans of film frames at Ruby), 31
 * lens open (the only time claps land), 32 rerun (calls up minions from
 * earlier pictures once below two thirds health). It never moves. */

#define C_SHUT 30
#define C_OPEN 31
#define C_RERUN 32

static void fire_film(struct Ent* e, int spread) {
  struct Ent* f = ent_alloc();
  if (!f) return;
  ent_setup(f, ENT_SHOT, 0, 0, 0, 2, 0, -1);
  f->x = e->x - (6 << 8);
  f->y = e->y + (12 << 8);
  int dx = (g_hero.x - f->x) >> 8;
  int dy = (g_hero.y - f->y) >> 8;
  if (dx > -8) dx = -8;
  /* normalize roughly to 2.5 px/tick using the larger axis */
  int ax = dx < 0 ? -dx : dx;
  int ay = dy < 0 ? -dy : dy;
  int big = ax > ay ? ax : ay;
  f->vx = (dx * 640) / big;
  f->vy = (dy * 640) / big + spread * 120;
  f->timer = 240;
}

static void step_camera(struct Ent* e) {
  e->anim++;
  switch (e->state) {
    case B_INTRO:
      g_pending_script = SCRIPT_FINAL_INTRO;
      e->state = C_SHUT;
      e->timer = 120;
      break;
    case C_SHUT:
      if ((e->timer % 36) == 0) {
        fire_film(e, 0);
        if (e->hp <= (CAMERA_HP * 2) / 3) {
          fire_film(e, -2);
          fire_film(e, 2);
        }
      }
      if (--e->timer <= 0) {
        e->state = C_OPEN;
        e->timer = 90;
      }
      break;
    case C_OPEN:
      if (--e->timer <= 0) {
        boss_cycle++;
        e->state = (e->hp <= (CAMERA_HP * 2) / 3 && (boss_cycle & 1)) ? C_RERUN : C_SHUT;
        e->timer = e->state == C_RERUN ? 30 : 120;
      }
      break;
    case C_RERUN:
      if (e->timer == 15) {
        struct Ent* m = ent_alloc();
        if (m) ent_setup(m, ENT_WALKER, 6, e->ty, 0, 0, 0, -1);
        struct Ent* f = ent_alloc();
        if (f) ent_setup(f, ENT_FLYER, 14, 10, 6, 0, 0, -1);
      }
      if (--e->timer <= 0) {
        e->state = C_SHUT;
        e->timer = 120;
      }
      break;
    default:
      break;
  }
}

static void step_boss(struct Ent* e) {
  if (e->a == 4) step_camera(e);
  else if (e->a == 3) step_kraken(e);
  else if (e->a == 2) step_horse(e);
  else step_sergeant(e);
}

/* Whether claps land on the boss now, and how hard. */
static int boss_hit_value(struct Ent* e) {
  if (e->a == 4) return e->state == C_OPEN ? 1 : 0;
  if (e->a == 3) return 0;     /* the puppeteer is hit through its tentacles */
  if (e->a == 2) return e->state == H_STOPPED ? 1 : 0;
  if (e->state == B_CHARGE || e->state == B_INTRO) return 0;
  return e->state == B_DAZED ? 2 : 1;
}

/* Whether touching the boss hurts Ruby. */
static int boss_contact_hurts(struct Ent* e) {
  if (e->a == 4) return 1;
  if (e->a == 3) return 0;
  if (e->a == 2) return e->state == H_RUN || e->state == H_BRAKE || e->state == H_LEAVE;
  return e->state != B_DAZED;
}

/* Clear encounter hazards before allocating rewards, even at entity capacity. */
static void boss_defeated(struct Ent* e) {
  int boss = e->a;
  int tx = (e->x >> 11) + 1;
  e->state = B_DOWN;
  e->alive = 0;
  e->kind = ENT_NONE;
  flag_set(BOSS_FLAG_BASE + e->a);
  g_events |= GE_BOSS_DOWN;
  if (boss == 4) g_events |= GE_FINALE;
  g_boss = 0;
  for (int i = 0; i < g_ent_count; i++) {
    if (g_ents[i].kind == ENT_FLYER || g_ents[i].kind == ENT_WAVE || g_ents[i].kind == ENT_SHOT ||
        g_ents[i].kind == ENT_TENTACLE || (boss == 4 && g_ents[i].kind == ENT_WALKER)) {
      g_ents[i].alive = 0;
      g_ents[i].kind = ENT_NONE;
    }
  }
  boss_rewards(boss, tx);
}

/* Move a ground shockwave until its leading edge hits scenery or its timer expires. */
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

/* A plate accepts Ruby or a projected body; plates on one channel are ORed. */
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

/* Resolve live and recorded clapper hits, then enemy contacts and any live pogo. */
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
    if (e->kind == ENT_TENTACLE) {
      struct Hitbox tb;
      ent_box(e, &tb);
      if (e->state == T_STUCK && !e->hurt && g_boss && g_boss->alive) {
        int hit = has_live && hitbox_overlap(&live, &tb);
        if (hit && g_hero.attack_dir == 3) pogo = 1;
        for (int s = 0; s < TAKE_MAX_SLOTS && !hit; s++) {
          if (has_film[s] && hitbox_overlap(&film[s], &tb)) hit = 1;
        }
        if (hit) {
          e->hurt = 16;
          g_boss->hp -= 1;
          g_events |= GE_HIT | GE_BOSS_HIT;
          if (g_boss->hp <= 0) {
            boss_defeated(g_boss);
            continue;
          }
        }
      }
      if ((e->state == T_RISE || e->state == T_HOVER || e->state == T_SLAM) && hitbox_overlap(&hb, &tb)) {
        hurt_hero(e);
      }
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
    if (!e->alive || (e->kind != ENT_SHOT && !is_enemy(e->kind))) continue;
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
    player_launch(&g_hero, POGO_SPEED);
  }
}

/* Collect overlapping rewards and process a single room-changing interaction. */
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
          player_launch(&g_hero, SPRING_SPEED);
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
            g_pending_script = e->a == ROOM_TOWER_BASE ? SCRIPT_TOWER_SEALED : SCRIPT_STAGE_SEALED;
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

/* Advance each live actor once and decrement its shared hit protection timer. */
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
      case ENT_TENTACLE: step_tentacle(e); break;
      case ENT_WAVE: step_wave(e); break;
      default:
        if (e->timer > 0 && e->kind != ENT_GATE) e->timer--;
        e->anim++;
        break;
    }
  }
}

/* ---- the tick ---- */

/* Run one deterministic 60-Hz step, resolving fatal contacts before interactions. */
void game_tick(int held, int pressed) {
  g_events = 0;
  g_prog.play_ticks++;
  takes_advance(&g_takes);
  step_movers();
  g_room.platform_count = fill_platforms(takes_platforms(&g_takes, g_platforms));
  g_room.platforms = g_platforms;
  player_step(&g_hero, &g_room, held, pressed);
  if (g_hero.events & EV_HAZARD) {
    hazard_respawn();
    if (g_health <= 0) die();
    return;
  }
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
  if (g_health <= 0) {
    die();
    return;
  }
  pickups_and_interactions(pressed);
  if (g_events & GE_ROOM) return;   /* a door moved us; the new room starts clean */
  if (g_health <= 0) {
    die();
    return;
  }
  check_edges();
  if (g_health <= 0) die();   /* a pit at a room's bottom edge */
}

/* ---- new game, save and load ---- */

/* Initialize all campaign progress and enter the chosen debug or normal start room. */
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

/* Load saved progress and enter its checkpoint, or return failure to the title menu. */
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
