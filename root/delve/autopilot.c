#include "delve.h"
#include "../crt/ps2.h"

/*
 * Autopilot: a simple player that plays DELVE by itself. It drives the
 * attract-mode demo behind the title screen and is handy for automated
 * play-testing.
 *
 * Every decision is a key the human could press, chosen from the player's
 * grid pose:
 *   - a monster in a straight, clear line ahead within range: cast at it;
 *   - a monster in a straight line to the side or behind: turn toward it;
 *   - otherwise walk the shortest path (BFS) to the most useful target: a
 *     potion when hurt, the rune key, crystals, the sealed door once the key
 *     is held, the portal, and finally the nearest unexplored frontier.
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

/* Direction (0..3) of an awake monster in a straight, unobstructed line
 * within `range` cells, nearest first, or -1. *out_dist gets its distance. */
static int monster_in_line(int cx, int cy, int range, int* out_dist){
  for (int d = 1; d <= range; ++d){
    for (int k = 0; k < 4; ++k){
      int x = cx + ap_dx[k] * d;
      int y = cy + ap_dy[k] * d;
      /* the line must be clear up to this cell */
      bool clear = true;
      for (int s = 1; s < d && clear; ++s){
        if (cell_solid(cx + ap_dx[k] * s, cy + ap_dy[k] * s)) clear = false;
      }
      if (!clear || cell_solid(x, y)) continue;
      struct Monster* m = monster_at(x, y);
      if (m != NULL){
        *out_dist = d;
        return k;
      }
    }
  }
  return -1;
}

/* Choose the next key for a player at (cx, cy) facing `facing`. */
int autopilot_decide(int cx, int cy, int facing, bool has_key, int mp){
  int mdist = 0;
  int mdir = monster_in_line(cx, cy, 6, &mdist);
  if (mdir >= 0){
    if (mdir == facing){
      if (mp >= 40 && mdist >= 2) return 'f';
      if (mp >= 5) return ' ';
      /* out of mana: back away if it is close */
      return mdist <= 1 ? 's' : 0;
    }
    if (mdir == ((facing + 1) & 3)) return 'e';
    return 'q';
  }
  int dir = path_step(cx, cy, has_key);
  if (dir < 0) return 'e';
  if (dir == facing) return 'w';
  if (dir == ((facing + 1) & 3)) return 'e';
  if (dir == ((facing + 3) & 3)) return 'q';
  return 'e';
}
