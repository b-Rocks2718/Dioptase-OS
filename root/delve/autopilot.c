#include "delve.h"

/*
 * Autopilot: a simple player that plays DELVE by itself. It drives the
 * attract-mode demo behind the title screen and is handy for automated
 * play-testing.
 *
 * Every tick it produces the same controls a human holds (struct Controls):
 *   - an awake monster in clear sight within range: turn to face it, cast
 *     when lined up, and back off when it comes close or mana runs dry;
 *   - a cracked wall beside it: face the wall and fireball it open;
 *   - otherwise walk toward the center of the next cell on the shortest path
 *     (BFS) to the most useful target: a potion when hurt, the rune key,
 *     crystals, the sealed door once the key is held, the portal, a seen
 *     cracked wall, and finally the nearest unexplored frontier.
 * If walking makes no progress for a while (a monster in a corridor, a
 * corner), it turns and sidesteps for a moment.
 */

static unsigned char dist[MAP_CELLS];
static short parent[MAP_CELLS];
static short queue[MAP_CELLS];

static int ap_dx[4] = {1, 0, -1, 0};
static int ap_dy[4] = {0, 1, 0, -1};

/* Whether the autopilot may walk through cell c. */
static bool walkable(int c, bool has_key){
  int v = g_map[c];
  if (v == CELL_EMPTY) return true;
  if (v < CELL_DOOR_BASE) return false;
  return !g_doors[v - CELL_DOOR_BASE].locked || has_key;
}

/* Whether item `it` is a goal in priority pass `pass` (0 urgent potion,
 * 1 key, 2 crystals, 3 the exit once the key is held, 4 any exit). */
static bool item_wanted(struct Item* it, int pass, bool has_key){
  switch (pass){
    case 0: return it->type == ITEM_POTION_HP && g_stats.hp < 50;
    case 1: return it->type == ITEM_KEY;
    case 2: return it->type == ITEM_CRYSTAL;
    case 3: return (it->type == ITEM_PORTAL || it->type == ITEM_HEART) && has_key;
    case 4: return it->type == ITEM_PORTAL || it->type == ITEM_HEART;
    default: return false;
  }
}

/* First step direction from (cx, cy) toward the nearest cell satisfying the
 * current goal, or -1. Goals are tried in priority order. */
static int path_step(int cx, int cy, bool has_key){
  int start = cy * MAP_W + cx;
  for (int i = 0; i < MAP_CELLS; ++i) dist[i] = 255;
  int head = 0;
  int tail = 0;
  dist[start] = 0;
  parent[start] = -1;
  queue[tail++] = start;
  while (head < tail){
    int c = queue[head++];
    int x = c & (MAP_W - 1);
    int y = c >> 5;
    for (int k = 0; k < 4; ++k){
      int nx = x + ap_dx[k];
      int ny = y + ap_dy[k];
      if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H) continue;
      int n = ny * MAP_W + nx;
      if (dist[n] != 255 || !walkable(n, has_key)) continue;
      if (monster_at(nx, ny) != NULL && dist[c] == 0) continue;
      dist[n] = dist[c] + 1 > 250 ? 250 : dist[c] + 1;
      parent[n] = c;
      queue[tail++] = n;
    }
  }
  int target = -1;
  for (int pass = 0; pass < 5 && target < 0; ++pass){
    int best = 255;
    for (int i = 0; i < MAX_ITEMS; ++i){
      struct Item* it = &g_items[i];
      if (!it->active || !item_wanted(it, pass, has_key)) continue;
      int c = it->cy * MAP_W + it->cx;
      if (!g_seen[c] || dist[c] >= best || dist[c] == 0) continue;
      best = dist[c];
      target = c;
    }
  }
  if (target < 0){
    /* a cell beside a cracked wall we have seen (a vault to blast open) */
    int best = 255;
    for (int c = 0; c < MAP_CELLS; ++c){
      if (g_map[c] != TEX_CRACKED + 1 || !g_seen[c]) continue;
      int x = c & (MAP_W - 1);
      int y = c >> 5;
      for (int k = 0; k < 4; ++k){
        int nx = x + ap_dx[k];
        int ny = y + ap_dy[k];
        if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H) continue;
        int n = ny * MAP_W + nx;
        if (dist[n] < best && dist[n] > 0){
          best = dist[n];
          target = n;
        }
      }
    }
  }
  if (target < 0){
    /* the nearest reachable cell next to unexplored space */
    for (int i = 1; i < tail && target < 0; ++i){
      int c = queue[i];
      int x = c & (MAP_W - 1);
      int y = c >> 5;
      for (int k = 0; k < 4; ++k){
        int nx = x + ap_dx[k];
        int ny = y + ap_dy[k];
        if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H) continue;
        if (!g_seen[ny * MAP_W + nx]){
          target = c;
          break;
        }
      }
    }
  }
  if (target < 0) return -1;
  int c = target;
  while (parent[c] != start && parent[c] >= 0) c = parent[c];
  int x = c & (MAP_W - 1);
  int y = c >> 5;
  for (int k = 0; k < 4; ++k){
    if (cx + ap_dx[k] == x && cy + ap_dy[k] == y) return k;
  }
  return -1;
}

/* Path state: the cell the current waypoint was planned from, and the
 * waypoint (the next cell's center). */
static int plan_cell = -1;
static int plan_age;
static int way_x;
static int way_y;
static bool way_valid;

/* Progress check: where the player was STUCK_TICKS ago, and the remaining
 * ticks of an evasive turn-and-sidestep. */
#define STUCK_TICKS 45
static int check_t;
static int check_x;
static int check_y;
static int evade_t;
static int evade_turn;

/* Turn rate in angle units per radian: 4096 / (2 pi). */
#define UNITS_PER_RADIAN 652

/* Steer toward world point (tx, ty): set c->turn to the angle units that
 * face it this tick (at most TURN_MAX). Returns how well lined up it is:
 * 2 within ~7 degrees, 1 within 45 degrees, 0 otherwise. */
static int face_point(int x, int y, int angle, int tx, int ty, struct Controls* c){
  int dir_x = icos(angle);
  int dir_y = isin(angle);
  int rx = tx - x;
  int ry = ty - y;
  int dot = fixmul(rx, dir_x) + fixmul(ry, dir_y);
  int cross = fixmul(dir_x, ry) - fixmul(dir_y, rx);
  int across = cross < 0 ? -cross : cross;
  int units = TURN_MAX;
  if (dot > 0 && across <= dot){
    /* atan(cross / dot) ~ cross / dot radians for the remaining 45 degrees */
    units = udiv32(UNITS_PER_RADIAN * across, dot);
    if (units > TURN_MAX) units = TURN_MAX;
  }
  c->turn = cross < 0 ? -units : units;
  if (dot > 0 && across * 8 < dot) return 2;
  if (dot > 0 && across < dot) return 1;
  return 0;
}

/* Nearest awake monster within `range` (16.16) in clear sight, or NULL. */
static struct Monster* visible_monster(int x, int y, int range, int* out_dist){
  struct Monster* best = NULL;
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (!m->alive || !m->awake) continue;
    int d = approx_dist(m->x - x, m->y - y);
    if (d >= range || !world_line_clear(x, y, m->x, m->y)) continue;
    range = d;
    best = m;
  }
  *out_dist = range;
  return best;
}

/* Fill `c` with this tick's controls for a player at (x, y) facing `angle`. */
void autopilot_steer(int x, int y, int angle, bool has_key, int mp, struct Controls* c){
  c->forward = 0;
  c->strafe = 0;
  c->turn = 0;
  c->run = false;
  c->cast = 0;
  int cx = x >> 16;
  int cy = y >> 16;

  if (evade_t > 0){
    evade_t -= 1;
    c->turn = evade_turn * TURN_MAX;
    c->strafe = evade_turn;
    c->forward = 1;
    return;
  }

  int mdist = 0;
  struct Monster* m = visible_monster(x, y, 0x70000, &mdist);
  if (m != NULL){
    int aim = face_point(x, y, angle, m->x, m->y, c);
    if (aim == 2){
      if (mp >= 40 && mdist >= 0x28000) c->cast = SHOT_FIRE + 1;
      else if (mp >= 5) c->cast = SHOT_SPARK + 1;
    }
    /* keep out of reach; with no mana left, fall back */
    if (mdist < MONSTER_REACH + 0x8000 || mp < 5) c->forward = -1;
    check_t = 0;
    return;
  }

  /* a cracked wall beside us: blast it open */
  if (mp >= 22){
    for (int k = 0; k < 4; ++k){
      int wx = cx + ap_dx[k];
      int wy = cy + ap_dy[k];
      if (wx < 0 || wy < 0 || wx >= MAP_W || wy >= MAP_H) continue;
      if (g_map[wy * MAP_W + wx] != TEX_CRACKED + 1) continue;
      if (face_point(x, y, angle, (wx << 16) + FIX_HALF, (wy << 16) + FIX_HALF, c) == 2){
        c->cast = SHOT_FIRE + 1;
      }
      check_t = 0;
      return;
    }
  }

  /* plan from each new cell (and now and then, as targets change) */
  int cell = cy * MAP_W + cx;
  plan_age += 1;
  if (cell != plan_cell || plan_age > 90){
    plan_cell = cell;
    plan_age = 0;
    int k = path_step(cx, cy, has_key);
    way_valid = k >= 0;
    if (way_valid){
      way_x = ((cx + ap_dx[k]) << 16) + FIX_HALF;
      way_y = ((cy + ap_dy[k]) << 16) + FIX_HALF;
    }
  }
  if (!way_valid){
    c->turn = TURN_MAX / 2;
    return;
  }
  if (face_point(x, y, angle, way_x, way_y, c) >= 1){
    c->forward = 1;
  }

  /* no progress while walking: turn and sidestep for a moment */
  check_t += 1;
  if (check_t >= STUCK_TICKS){
    if (c->forward != 0 && approx_dist(x - check_x, y - check_y) < 0x2000){
      evade_t = 20;
      evade_turn = rng_range(2) ? 1 : -1;
      plan_cell = -1;
    }
    check_t = 0;
    check_x = x;
    check_y = y;
  }
}

/* Forget the current plan (a new level, or the human handed over control). */
void autopilot_reset(void){
  plan_cell = -1;
  way_valid = false;
  evade_t = 0;
  check_t = 0;
}
