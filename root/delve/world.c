#include "delve.h"

/*
 * The dungeon: procedural level generation, map queries, doors, monsters,
 * items, projectiles, and short-lived effects.
 *
 * All simulation runs in 60 Hz ticks (world_tick). Creatures live on the grid
 * like the player: they occupy one cell at a time and slide to a neighbor
 * over a few ticks. Projectiles move freely in 16.16 world space.
 */

unsigned char g_map[MAP_CELLS];
unsigned char g_seen[MAP_CELLS];       /* cells the player has seen (automap) */
static unsigned char occupant[MAP_CELLS]; /* monster index + 1, 0 = none */
static unsigned char flow[MAP_CELLS];  /* BFS steps to the player, 255 = none */
static bool map_changed;               /* a wall fell: rebuild the flow field */

struct Door g_doors[MAX_DOORS];
int g_door_count;
struct Monster g_monsters[MAX_MONSTERS];
struct Item g_items[MAX_ITEMS];
struct Projectile g_shots[MAX_SHOTS];
struct Effect g_effects[MAX_EFFECTS];
struct Light g_lights[MAX_LIGHTS];
int g_light_count;
static struct Torch torches[MAX_TORCHES];
static int torch_count;
static struct Prop props[MAX_PROPS];
static int prop_count;
struct Particle g_particles[MAX_PARTICLES];
static int next_particle;
struct PlayerStats g_stats;

/* Base wall texture and the floor/ceiling for the current level. */
int g_floor_flat;
int g_ceiling_flat;

/* ---- Random numbers (xorshift32) ---- */
static unsigned rng_state = 0x1234567;

/* Reseed the level generator's random stream (0 picks a fixed seed). */
void rng_seed(unsigned seed){
  rng_state = seed ? seed : 0x1234567;
}

/* Next 32-bit value from the xorshift32 stream. */
unsigned rng_next(void){
  unsigned x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

/* Uniform integer in [0, n) for small n. */
int rng_range(int n){
  return (int)((rng_next() >> 8) % (unsigned)n);
}

/* ---- Monster stats ---- */
struct MonsterKind {
  int hp;
  int step_ticks;     /* ticks per cell moved */
  int think_ticks;    /* pause between decisions */
  int damage;
  int ranged;         /* shoots instead of biting */
  int attack_ticks;   /* cooldown after attacking */
  int sprite;
  int frames;
  int size;           /* world height 16.16 */
  int z;              /* bottom above floor 16.16 */
  int score;
  int aggro;          /* flow distance at which it notices the player */
};

static struct MonsterKind kinds[MONSTER_KINDS] = {
  /* hp step think dmg ranged atk  sprite      frames size   z      score aggro */
  {  4, 22,  10,  8,  0,  45, SPR_SLIME,   4, 46000,     0, 10,  9 },
  {  2,  9,   4,  5,  0,  30, SPR_BAT,     4, 30000, 22000, 15, 12 },
  {  5, 26,  20, 10,  1,  90, SPR_EYE,     3, 42000, 10000, 25, 11 },
  {  8, 14,   8, 14,  0,  40, SPR_WRAITH,  4, 52000,  4000, 40, 14 },
  { 36, 30,  14, 16,  2,  80, SPR_WARDEN,  2, 62000,  2000, 500, 30 },
};
#define WARDEN_CHARGE_TICKS 28

/* ---- Map helpers ---- */

static int cell_at(int cx, int cy){
  return cy * MAP_W + cx;
}

/* Whether cell (cx, cy) is inside the 32x32 map. */
static bool in_map(int cx, int cy){
  return cx >= 0 && cy >= 0 && cx < MAP_W && cy < MAP_H;
}

/* Door record for a door cell, or NULL. */
struct Door* door_at(int cx, int cy){
  if (!in_map(cx, cy)) return NULL;
  int c = g_map[cell_at(cx, cy)];
  if (c < CELL_DOOR_BASE) return NULL;
  return &g_doors[c - CELL_DOOR_BASE];
}

/* Whether cell (cx, cy) blocks movement: walls, closed or locked doors, and
 * anything outside the map. */
bool cell_solid(int cx, int cy){
  if (!in_map(cx, cy)) return true;
  int c = g_map[cell_at(cx, cy)];
  if (c == CELL_EMPTY) return false;
  if (c >= CELL_DOOR_BASE){
    struct Door* d = &g_doors[c - CELL_DOOR_BASE];
    return d->open < DOOR_PASSABLE;
  }
  return true;
}

/* Whether a projectile at world position (x, y) hits something solid. Doors
 * are thin: only their center plane, minus the opened part, blocks. */
static bool point_solid(int x, int y){
  int cx = x >> 16;
  int cy = y >> 16;
  if (!in_map(cx, cy)) return true;
  int c = g_map[cell_at(cx, cy)];
  if (c == CELL_EMPTY) return false;
  if (c < CELL_DOOR_BASE) return true;
  struct Door* d = &g_doors[c - CELL_DOOR_BASE];
  int along = d->axis_x ? (y & 0xFFFF) : (x & 0xFFFF);
  int across = d->axis_x ? (x & 0xFFFF) : (y & 0xFFFF);
  if (across < FIX_HALF - 0x1800 || across > FIX_HALF + 0x1800) return false;
  return along >= d->open;
}

/* Begin opening the door at (cx, cy). Returns false for a locked door. */
bool door_open(int cx, int cy){
  struct Door* d = door_at(cx, cy);
  if (d == NULL) return true;
  if (d->locked) return false;
  if (d->open < FIX_ONE && !d->opening){
    d->opening = 1;
    sfx(SFX_DOOR);
  }
  return true;
}

/* Number of living monsters on the level. */
static int monster_count_alive(void){
  int n = 0;
  for (int i = 0; i < MAX_MONSTERS; ++i){
    if (g_monsters[i].alive) n += 1;
  }
  return n;
}

/* Monster standing on or moving into (cx, cy), or NULL. */
struct Monster* monster_at(int cx, int cy){
  if (!in_map(cx, cy)) return NULL;
  int o = occupant[cell_at(cx, cy)];
  return o ? &g_monsters[o - 1] : NULL;
}

/* ---- Level generation ---- */

static void world_layout(int depth, int* sx, int* sy, int* sdir);
static void add_item(int type, int cx, int cy);

struct Room {
  int x;
  int y;
  int w;
  int h;
};

#define MAX_ROOMS 10
static struct Room rooms[MAX_ROOMS];
static int room_count;

/* Whether a candidate room (with a one-cell margin) touches an existing one. */
static bool room_overlaps(int x, int y, int w, int h){
  for (int i = 0; i < room_count; ++i){
    struct Room* r = &rooms[i];
    if (x - 1 < r->x + r->w && x + w + 1 > r->x &&
        y - 1 < r->y + r->h && y + h + 1 > r->y){
      return true;
    }
  }
  return false;
}

/* Open one cell, never touching the outer boundary walls. */
static void carve(int cx, int cy){
  if (cx > 0 && cy > 0 && cx < MAP_W - 1 && cy < MAP_H - 1){
    g_map[cell_at(cx, cy)] = CELL_EMPTY;
  }
}

static int room_center_x(int i){ return rooms[i].x + rooms[i].w / 2; }
static int room_center_y(int i){ return rooms[i].y + rooms[i].h / 2; }

/* L-shaped corridor between two room centers. */
static void connect_rooms(int a, int b){
  int x0 = room_center_x(a);
  int y0 = room_center_y(a);
  int x1 = room_center_x(b);
  int y1 = room_center_y(b);
  bool horizontal_first = rng_range(2) == 0;
  int x = x0;
  int y = y0;
  if (horizontal_first){
    while (x != x1){ carve(x, y); x += x1 > x ? 1 : -1; }
    while (y != y1){ carve(x, y); y += y1 > y ? 1 : -1; }
  } else {
    while (y != y1){ carve(x, y); y += y1 > y ? 1 : -1; }
    while (x != x1){ carve(x, y); x += x1 > x ? 1 : -1; }
  }
  carve(x1, y1);
}

/* Whether (cx, cy) lies inside any room (as opposed to a corridor). */
static bool is_room_cell(int cx, int cy){
  for (int i = 0; i < room_count; ++i){
    struct Room* r = &rooms[i];
    if (cx >= r->x && cx < r->x + r->w && cy >= r->y && cy < r->y + r->h){
      return true;
    }
  }
  return false;
}

/* Index of the room containing (cx, cy), or -1. */
static int room_of(int cx, int cy){
  for (int i = 0; i < room_count; ++i){
    struct Room* r = &rooms[i];
    if (cx >= r->x && cx < r->x + r->w && cy >= r->y && cy < r->y + r->h){
      return i;
    }
  }
  return -1;
}

/* Turn cell (cx, cy) into a closed door; returns its index or -1 if full. */
static int add_door(int cx, int cy, bool axis_x, bool locked){
  if (g_door_count >= MAX_DOORS) return -1;
  struct Door* d = &g_doors[g_door_count];
  d->cx = cx;
  d->cy = cy;
  d->axis_x = axis_x;
  d->locked = locked;
  d->open = 0;
  d->opening = 0;
  g_map[cell_at(cx, cy)] = CELL_DOOR_BASE + g_door_count;
  g_door_count += 1;
  return g_door_count - 1;
}

/* Whether every cell around the w x h block at (x, y), including the block,
 * is solid wall (so a vault carved there touches nothing open). */
static bool block_buried(int x, int y, int w, int h){
  for (int cy = y - 1; cy <= y + h; ++cy){
    for (int cx = x - 1; cx <= x + w; ++cx){
      if (!in_map(cx, cy) || cx == 0 || cy == 0 || cx == MAP_W - 1 || cy == MAP_H - 1){
        if (cx >= x && cx < x + w && cy >= y && cy < y + h) return false;
        continue;
      }
      int v = g_map[cell_at(cx, cy)];
      if (v == CELL_EMPTY || v >= CELL_DOOR_BASE) return false;
    }
  }
  return true;
}

/* Hide a 2x2 vault of crystals behind a cracked wall of some room. The
 * cracked cell is the only way in; a fireball breaks it (see explode()). */
static void place_vault(void){
  for (int tries = 0; tries < 60; ++tries){
    int r = rng_range(room_count);
    int dir = rng_range(4);
    int dx = (dir == 0) - (dir == 2);
    int dy = (dir == 1) - (dir == 3);
    /* a wall cell just outside the room on side `dir` */
    int wx;
    int wy;
    if (dx != 0){
      wx = dx > 0 ? rooms[r].x + rooms[r].w : rooms[r].x - 1;
      wy = rooms[r].y + rng_range(rooms[r].h);
    } else {
      wy = dy > 0 ? rooms[r].y + rooms[r].h : rooms[r].y - 1;
      wx = rooms[r].x + rng_range(rooms[r].w);
    }
    if (!in_map(wx, wy) || g_map[cell_at(wx, wy)] == CELL_EMPTY ||
        g_map[cell_at(wx, wy)] >= CELL_DOOR_BASE) continue;
    /* the vault's near corner sits beyond the wall */
    int vx = wx + dx;
    int vy = wy + dy;
    int bx = dx < 0 ? vx - 1 : vx;
    int by = dy < 0 ? vy - 1 : vy;
    if (dx == 0) bx = vx - rng_range(2);
    if (dy == 0) by = vy - rng_range(2);
    if (!block_buried(bx, by, 2, 2)) continue;
    /* the cracked wall's other neighbors must stay solid too */
    if (!cell_solid(wx + dy, wy + dx) || !cell_solid(wx - dy, wy - dx)) continue;
    for (int y = by; y < by + 2; ++y){
      for (int x = bx; x < bx + 2; ++x){
        g_map[cell_at(x, y)] = CELL_EMPTY;
      }
    }
    g_map[cell_at(wx, wy)] = TEX_CRACKED + 1;
    add_item(ITEM_CRYSTAL, bx, by);
    add_item(ITEM_CRYSTAL, bx + 1, by + 1);
    add_item(ITEM_CRYSTAL, bx + 1, by);
    add_item(rng_range(2) ? ITEM_POTION_HP : ITEM_POTION_MP, bx, by + 1);
    return;
  }
}

/* Mount one or two torches on the walls of most rooms. A torch hangs just in
 * front of the wall face, so it is placed in the room cell next to the wall
 * and pushed toward it. */
static void place_torches(void){
  torch_count = 0;
  for (int r = 0; r < room_count && torch_count < MAX_TORCHES; ++r){
    int want = 1 + (rooms[r].w * rooms[r].h > 20);
    for (int tries = 0; tries < 20 && want > 0 && torch_count < MAX_TORCHES; ++tries){
      int x = rooms[r].x + rng_range(rooms[r].w);
      int y = rooms[r].y + rng_range(rooms[r].h);
      int dir = rng_range(4);
      int wx = x + (dir == 0) - (dir == 2);
      int wy = y + (dir == 1) - (dir == 3);
      if (!in_map(wx, wy)) continue;
      int w = g_map[cell_at(wx, wy)];
      if (w == CELL_EMPTY || w >= CELL_DOOR_BASE) continue;
      struct Torch* t = &torches[torch_count++];
      t->x = (x << 16) + FIX_HALF + ((wx - x) * 0x7000);
      t->y = (y << 16) + FIX_HALF + ((wy - y) * 0x7000);
      t->phase = rng_range(64);
      want -= 1;
    }
  }
}

/* Put doors where corridors leave rooms, if the doorway is a clean 1-wide gap. */
static void place_doors(void){
  for (int cy = 1; cy < MAP_H - 1; ++cy){
    for (int cx = 1; cx < MAP_W - 1; ++cx){
      if (g_map[cell_at(cx, cy)] != CELL_EMPTY || is_room_cell(cx, cy)) continue;
      bool walls_ns = cell_solid(cx, cy - 1) && cell_solid(cx, cy + 1);
      bool walls_ew = cell_solid(cx - 1, cy) && cell_solid(cx + 1, cy);
      bool room_ew = is_room_cell(cx - 1, cy) || is_room_cell(cx + 1, cy);
      bool room_ns = is_room_cell(cx, cy - 1) || is_room_cell(cx, cy + 1);
      if (walls_ns && room_ew && !walls_ew && rng_range(100) < 45){
        /* corridor runs east-west: the door plane faces along x */
        add_door(cx, cy, true, false);
      } else if (walls_ew && room_ns && !walls_ns && rng_range(100) < 45){
        add_door(cx, cy, false, false);
      }
    }
  }
}

/* Breadth-first distances from (sx, sy) over passable cells. Doors count as
 * passable (creatures open them). Writes `out`, 255 = unreachable. */
static void bfs_from(int sx, int sy, unsigned char* out, bool through_locked){
  static short queue[MAP_CELLS];
  for (int i = 0; i < MAP_CELLS; ++i) out[i] = 255;
  int head = 0;
  int tail = 0;
  int start = cell_at(sx, sy);
  out[start] = 0;
  queue[tail++] = start;
  while (head < tail){
    int c = queue[head++];
    int d = out[c];
    if (d >= 254) continue;
    int cx = c & (MAP_W - 1);
    int cy = c >> 5;
    for (int k = 0; k < 4; ++k){
      int nx = cx + (k == 0) - (k == 2);
      int ny = cy + (k == 1) - (k == 3);
      if (!in_map(nx, ny)) continue;
      int n = cell_at(nx, ny);
      if (out[n] != 255) continue;
      int v = g_map[n];
      if (v != CELL_EMPTY){
        if (v < CELL_DOOR_BASE) continue;
        if (g_doors[v - CELL_DOOR_BASE].locked && !through_locked) continue;
      }
      out[n] = d + 1;
      queue[tail++] = n;
    }
  }
}

/* A random open, unoccupied floor cell inside room `r` (not a doorway). */
static bool random_room_cell(int r, int* ox, int* oy){
  for (int tries = 0; tries < 30; ++tries){
    int x = rooms[r].x + rng_range(rooms[r].w);
    int y = rooms[r].y + rng_range(rooms[r].h);
    int c = cell_at(x, y);
    if (g_map[c] != CELL_EMPTY || occupant[c]) continue;
    bool taken = false;
    for (int i = 0; i < MAX_ITEMS; ++i){
      if (g_items[i].active && g_items[i].cx == x && g_items[i].cy == y) taken = true;
    }
    if (taken) continue;
    *ox = x;
    *oy = y;
    return true;
  }
  return false;
}

/* Place an item in the first free slot (silently dropped if all are used). */
static void add_item(int type, int cx, int cy){
  for (int i = 0; i < MAX_ITEMS; ++i){
    if (!g_items[i].active){
      g_items[i].active = 1;
      g_items[i].type = type;
      g_items[i].cx = cx;
      g_items[i].cy = cy;
      g_items[i].phase = rng_range(64);
      return;
    }
  }
}

/* Spawn a sleeping monster of `kind` at (cx, cy), tougher on deep levels. */
static void add_monster(int kind, int cx, int cy){
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (m->alive) continue;
    m->alive = 1;
    m->kind = kind;
    m->hp = kinds[kind].hp + (g_stats.depth > 4 ? (g_stats.depth - 4) / 2 : 0);
    m->cx = cx;
    m->cy = cy;
    m->x = (cx << 16) + FIX_HALF;
    m->y = (cy << 16) + FIX_HALF;
    m->move_t = 0;
    m->move_len = 0;
    m->cooldown = 30 + rng_range(60);
    m->attack_t = 0;
    m->flash = 0;
    m->anim = rng_range(64);
    m->awake = 0;
    occupant[cell_at(cx, cy)] = i + 1;
    return;
  }
}

/* Texture theme per depth: base walls, accent walls, floor, ceiling. */
static int theme_base[6] = { TEX_BRICK, TEX_BRICK_MOSS, TEX_ROCK, TEX_BRICK_DARK, TEX_ROCK_GEM, TEX_RUNES };
static int theme_accent[6] = { TEX_BRICK_MOSS, TEX_BRICK, TEX_ROCK_GEM, TEX_RUNES, TEX_ROCK, TEX_BRICK_DARK };

/* Build depth `depth` from `seed`. Places the player start in *sx, *sy
 * facing *sdir (0 east .. 3 north). */
void world_generate(int depth, unsigned seed, int* sx, int* sy, int* sdir){
  rng_seed(seed * 2654435761u + depth * 40503u + 7);
  world_layout(depth, sx, sy, sdir);
}

/* Lay out one level from the current random stream. Retries with the stream
 * where it left off if too few rooms fit (the generator below assumes a
 * start room, an exit room, and at least one other). */
static void world_layout(int depth, int* sx, int* sy, int* sdir){
  int theme = (depth - 1) % 6;
  int base = theme_base[theme] + 1;
  int accent = theme_accent[theme] + 1;
  g_floor_flat = (theme == 1 || theme == 4) ? FLAT_FLOOR_MOSS : FLAT_FLOOR_STONE;
  g_ceiling_flat = (theme >= 2) ? FLAT_CEILING_ROCK : FLAT_CEILING_BEAMS;

  for (int i = 0; i < MAP_CELLS; ++i){
    g_map[i] = base;
    g_seen[i] = 0;
    occupant[i] = 0;
  }
  for (int i = 0; i < MAX_MONSTERS; ++i) g_monsters[i].alive = 0;
  for (int i = 0; i < MAX_ITEMS; ++i) g_items[i].active = 0;
  for (int i = 0; i < MAX_SHOTS; ++i) g_shots[i].active = 0;
  for (int i = 0; i < MAX_EFFECTS; ++i) g_effects[i].active = 0;
  for (int i = 0; i < MAX_PARTICLES; ++i) g_particles[i].life = 0;
  g_door_count = 0;

  /* rooms */
  room_count = 0;
  for (int tries = 0; tries < 200 && room_count < MAX_ROOMS; ++tries){
    int w = 3 + rng_range(4);
    int h = 3 + rng_range(4);
    int x = 1 + rng_range(MAP_W - w - 2);
    int y = 1 + rng_range(MAP_H - h - 2);
    if (room_overlaps(x, y, w, h)) continue;
    rooms[room_count].x = x;
    rooms[room_count].y = y;
    rooms[room_count].w = w;
    rooms[room_count].h = h;
    room_count += 1;
  }
  if (room_count < 3){
    world_layout(depth, sx, sy, sdir);
    return;
  }
  for (int i = 0; i < room_count; ++i){
    for (int y = rooms[i].y; y < rooms[i].y + rooms[i].h; ++y){
      for (int x = rooms[i].x; x < rooms[i].x + rooms[i].w; ++x){
        carve(x, y);
      }
    }
  }
  /* spanning tree plus a couple of loops */
  for (int i = 1; i < room_count; ++i){
    connect_rooms(i, rng_range(i));
  }
  for (int i = 0; i < 2 && room_count > 3; ++i){
    connect_rooms(rng_range(room_count), rng_range(room_count));
  }

  /* accent walls: every wall touching room 1, 3, 5... uses the accent */
  for (int cy = 1; cy < MAP_H - 1; ++cy){
    for (int cx = 1; cx < MAP_W - 1; ++cx){
      int c = cell_at(cx, cy);
      if (g_map[c] == CELL_EMPTY) continue;
      for (int k = 0; k < 4; ++k){
        int nx = cx + (k == 0) - (k == 2);
        int ny = cy + (k == 1) - (k == 3);
        int r = room_of(nx, ny);
        if (r >= 0 && (r & 1)) g_map[c] = accent;
      }
      if (g_map[c] == base && rng_range(100) < 4) g_map[c] = TEX_ROCK_GEM + 1;
    }
  }

  place_doors();

  /* start in room 0 facing its longer open direction */
  int start_room = 0;
  *sx = room_center_x(start_room);
  *sy = room_center_y(start_room);
  *sdir = rng_range(4);
  for (int k = 0; k < 4; ++k){
    int d = (*sdir + k) & 3;
    int nx = *sx + (d == 0) - (d == 2);
    int ny = *sy + (d == 1) - (d == 3);
    if (!cell_solid(nx, ny)){
      *sdir = d;
      break;
    }
  }

  /* exit: farthest room; carve a sealed alcove (locked door + portal) off it */
  bfs_from(*sx, *sy, flow, true);
  int exit_room = room_count - 1;
  int best = -1;
  for (int i = 1; i < room_count; ++i){
    int d = flow[cell_at(room_center_x(i), room_center_y(i))];
    if (d != 255 && d > best){
      best = d;
      exit_room = i;
    }
  }
  bool placed = false;
  for (int attempt = 0; attempt < 40 && !placed; ++attempt){
    struct Room* r = &rooms[exit_room];
    int side = rng_range(4);
    int dx = (side == 0) - (side == 2);
    int dy = (side == 1) - (side == 3);
    int ex;
    int ey;
    if (dx != 0){
      ex = dx > 0 ? r->x + r->w : r->x - 1;
      ey = r->y + rng_range(r->h);
    } else {
      ey = dy > 0 ? r->y + r->h : r->y - 1;
      ex = r->x + rng_range(r->w);
    }
    int px = ex + dx;
    int py = ey + dy;
    if (px < 1 || py < 1 || px >= MAP_W - 1 || py >= MAP_H - 1) continue;
    /* door cell and portal cell must be solid and enclosed by solid sides */
    if (g_map[cell_at(ex, ey)] == CELL_EMPTY || g_map[cell_at(px, py)] == CELL_EMPTY) continue;
    if (g_map[cell_at(ex, ey)] >= CELL_DOOR_BASE || g_map[cell_at(px, py)] >= CELL_DOOR_BASE) continue;
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k){
      int nx = px + (k == 0) - (k == 2);
      int ny = py + (k == 1) - (k == 3);
      if (nx == ex && ny == ey) continue;
      if (!in_map(nx, ny) || g_map[cell_at(nx, ny)] == CELL_EMPTY ||
          g_map[cell_at(nx, ny)] >= CELL_DOOR_BASE) ok = false;
    }
    int sx1 = ex + dy;
    int sy1 = ey + dx;
    int sx2 = ex - dy;
    int sy2 = ey - dx;
    if (!in_map(sx1, sy1) || !in_map(sx2, sy2) ||
        g_map[cell_at(sx1, sy1)] == CELL_EMPTY || g_map[cell_at(sx2, sy2)] == CELL_EMPTY){
      ok = false;
    }
    if (!ok) continue;
    g_map[cell_at(px, py)] = CELL_EMPTY;
    g_map[cell_at(ex, ey)] = CELL_EMPTY;
    add_door(ex, ey, dx != 0, true);
    add_item(depth >= FINAL_DEPTH ? ITEM_HEART : ITEM_PORTAL, px, py);
    /* the alcove walls glow with runes */
    for (int k = 0; k < 4; ++k){
      int nx = px + (k == 0) - (k == 2);
      int ny = py + (k == 1) - (k == 3);
      if (g_map[cell_at(nx, ny)] != CELL_EMPTY && g_map[cell_at(nx, ny)] < CELL_DOOR_BASE){
        g_map[cell_at(nx, ny)] = TEX_RUNES + 1;
      }
    }
    placed = true;
  }
  if (!placed){
    /* no room for an alcove: put the portal in the exit room behind no door */
    int x;
    int y;
    if (random_room_cell(exit_room, &x, &y)) add_item(depth >= FINAL_DEPTH ? ITEM_HEART : ITEM_PORTAL, x, y);
  }

  place_torches();
  if (depth >= 2 || rng_range(2)) place_vault();

  /* floor clutter: mostly bones, mushrooms deeper down */
  prop_count = 0;
  int clutter = 3 + rng_range(4);
  for (int i = 0; i < clutter && prop_count < MAX_PROPS; ++i){
    int x;
    int y;
    if (!random_room_cell(rng_range(room_count), &x, &y)) continue;
    props[prop_count].cx = x;
    props[prop_count].cy = y;
    props[prop_count].kind = (depth >= 3 && rng_range(2)) ? 1 : 0;
    prop_count += 1;
  }

  /* key in a room reachable without the locked door, away from the start */
  bfs_from(*sx, *sy, flow, false);
  int key_room = -1;
  int key_best = -1;
  for (int i = 1; i < room_count; ++i){
    int d = flow[cell_at(room_center_x(i), room_center_y(i))];
    if (d == 255 || i == exit_room) continue;
    int score = d + rng_range(12);
    if (score > key_best){
      key_best = score;
      key_room = i;
    }
  }
  if (depth >= FINAL_DEPTH){
    /* the Warden waits in the exit room with the key */
    int x;
    int y;
    if (random_room_cell(exit_room, &x, &y)){
      add_monster(MON_WARDEN, x, y);
    } else if (random_room_cell(key_room < 0 ? exit_room : key_room, &x, &y)){
      add_item(ITEM_KEY, x, y);
    }
  } else if (placed){
    int x;
    int y;
    if (key_room < 0) key_room = exit_room;
    if (random_room_cell(key_room, &x, &y)) add_item(ITEM_KEY, x, y);
  }

  /* crystals and potions */
  int crystals = 7 + rng_range(4);
  for (int i = 0; i < crystals; ++i){
    int x;
    int y;
    if (random_room_cell(1 + rng_range(room_count - 1), &x, &y)) add_item(ITEM_CRYSTAL, x, y);
  }
  int potions = 2 + rng_range(2);
  for (int i = 0; i < potions; ++i){
    int x;
    int y;
    if (random_room_cell(rng_range(room_count), &x, &y)){
      add_item(rng_range(3) == 0 ? ITEM_POTION_MP : ITEM_POTION_HP, x, y);
    }
  }

  /* monsters, never in the start room */
  int count = 3 + depth + depth / 2;
  if (count > MAX_MONSTERS - 2) count = MAX_MONSTERS - 2;
  for (int i = 0; i < count; ++i){
    int r = 1 + rng_range(room_count - 1);
    int x;
    int y;
    if (!random_room_cell(r, &x, &y)) continue;
    int roll = rng_range(100);
    int kind = MON_SLIME;
    if (depth >= 2 && roll < 30) kind = MON_BAT;
    if (depth >= 3 && roll >= 30 && roll < 55) kind = MON_EYE;
    if (depth >= 4 && roll >= 80) kind = MON_WRAITH;
    if (depth == 1 && roll < 25) kind = MON_BAT;
    add_monster(kind, x, y);
  }

  bfs_from(*sx, *sy, flow, false);
}

/* ---- Effects and lights ---- */

void spawn_effect(int kind, int x, int y, int z){
  for (int i = 0; i < MAX_EFFECTS; ++i){
    struct Effect* e = &g_effects[i];
    if (e->active) continue;
    e->active = 1;
    e->kind = kind;
    e->x = x;
    e->y = y;
    e->z = z;
    e->t = 0;
    return;
  }
}

/* Throw `count` sparks from (x, y, z) in random directions. */
void spawn_burst(int x, int y, int z, int count, int color, int speed){
  for (int i = 0; i < count; ++i){
    struct Particle* p = &g_particles[next_particle];
    next_particle = (next_particle + 1) % MAX_PARTICLES;
    int a = rng_range(ANGLE_COUNT);
    int s = (speed >> 1) + rng_range(speed >> 1);
    p->x = x;
    p->y = y;
    p->z = z;
    p->vx = fixmul(icos(a), s);
    p->vy = fixmul(isin(a), s);
    p->vz = (rng_range(speed) + (speed >> 1));
    p->life = 18 + rng_range(18);
    p->color = color;
  }
}

/* Move sparks one tick: walls reflect them, the floor bounces them. */
static void update_particles(void){
  for (int i = 0; i < MAX_PARTICLES; ++i){
    struct Particle* p = &g_particles[i];
    if (p->life <= 0) continue;
    p->life -= 1;
    int nx = p->x + p->vx;
    int ny = p->y + p->vy;
    /* bounce off walls by reversing the blocked axis */
    if (cell_solid(nx >> 16, p->y >> 16)){
      p->vx = -(p->vx >> 1);
      nx = p->x;
    }
    if (cell_solid(p->x >> 16, ny >> 16)){
      p->vy = -(p->vy >> 1);
      ny = p->y;
    }
    p->x = nx;
    p->y = ny;
    p->z += p->vz;
    p->vz -= 0x140;
    if (p->z < 0){
      p->z = 0;
      p->vz = -(p->vz >> 1);
      p->vx = p->vx >> 1;
      p->vy = p->vy >> 1;
    }
  }
}

/* Add a point light for this frame (one level per half cell of radius);
 * extra lights beyond MAX_LIGHTS are dropped. */
static void add_light(int x, int y, int radius){
  if (g_light_count >= MAX_LIGHTS) return;
  struct Light* l = &g_lights[g_light_count++];
  l->x = x;
  l->y = y;
  l->radius = radius;
}

/* ---- Projectiles ---- */

/* Launch a projectile from (x, y) with velocity (vx, vy) per tick. */
void spawn_shot(int kind, int x, int y, int vx, int vy, bool from_player){
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Projectile* p = &g_shots[i];
    if (p->active) continue;
    p->active = 1;
    p->kind = kind;
    p->x = x;
    p->y = y;
    p->vx = vx;
    p->vy = vy;
    p->from_player = from_player;
    p->life = 180;
    return;
  }
}

/* Cheap distance estimate: max + min / 2 (octagonal, within ~12%). */
int approx_dist(int dx, int dy){
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}

/* Hurt a monster; on death award score, burst, and maybe drop loot (the
 * Warden always drops the rune key). */
static void damage_monster(struct Monster* m, int amount){
  m->hp -= amount;
  m->flash = 8;
  m->awake = 1;
  if (m->hp <= 0){
    m->alive = 0;
    occupant[cell_at(m->cx, m->cy)] = 0;
    g_stats.score += kinds[m->kind].score * g_stats.depth;
    g_stats.kills += 1;
    spawn_effect(EFFECT_GEM_PUFF, m->x, m->y, kinds[m->kind].z + kinds[m->kind].size / 2);
    spawn_burst(m->x, m->y, kinds[m->kind].z + kinds[m->kind].size / 2, 10, 0x08F6, 0x1000);
    sfx(SFX_MONSTER_DIE);
    if (m->kind == MON_WARDEN){
      /* the Warden carries the rune key to the Heart's seal */
      add_item(ITEM_KEY, m->cx, m->cy);
      spawn_burst(m->x, m->y, FIX_HALF, 24, 0x0BFA, 0x1800);
      spawn_effect(EFFECT_FIRE_PUFF, m->x, m->y, FIX_HALF);
      sfx(SFX_EXPLODE);
      g_stats.shake = 20;
    } else if (rng_range(100) < 35){
      /* slain creatures sometimes leave a crystal behind */
      add_item(ITEM_CRYSTAL, m->cx, m->cy);
    }
  } else {
    sfx(SFX_MONSTER_HURT);
  }
}

/* Detonate a projectile where it stopped: burst, sparks, and for fireballs
 * splash damage within about 1.25 cells plus a camera shake. */
static void explode(struct Projectile* p){
  p->active = 0;
  int kind = p->kind;
  int effect = kind == SHOT_ENEMY ? EFFECT_PINK_PUFF : (kind == SHOT_FIRE ? EFFECT_FIRE_PUFF : EFFECT_SPARK_PUFF);
  spawn_effect(effect, p->x, p->y, FIX_HALF - 0x2000);
  int color = kind == SHOT_ENEMY ? 0x0B5F : (kind == SHOT_FIRE ? 0x03AF : 0x0FE8);
  spawn_burst(p->x - (p->vx >> 1), p->y - (p->vy >> 1), FIX_HALF - 0x1000,
              kind == SHOT_FIRE ? 14 : 7, color, kind == SHOT_FIRE ? 0x1400 : 0x0C00);
  if (kind == SHOT_FIRE){
    sfx(SFX_EXPLODE);
    g_stats.shake = 10;
    /* the blast shatters a cracked wall next to it */
    int ex = p->x >> 16;
    int ey = p->y >> 16;
    for (int k = 0; k < 5; ++k){
      int cx = ex + (k == 1) - (k == 2);
      int cy = ey + (k == 3) - (k == 4);
      if (!in_map(cx, cy) || g_map[cell_at(cx, cy)] != TEX_CRACKED + 1) continue;
      if (approx_dist(((cx << 16) + FIX_HALF) - p->x, ((cy << 16) + FIX_HALF) - p->y) > 0x13000) continue;
      g_map[cell_at(cx, cy)] = CELL_EMPTY;
      map_changed = true;
      spawn_burst((cx << 16) + FIX_HALF, (cy << 16) + FIX_HALF, FIX_HALF, 20, 0x0668, 0x1400);
      spawn_effect(EFFECT_GEM_PUFF, (cx << 16) + FIX_HALF, (cy << 16) + FIX_HALF, FIX_HALF - 0x2000);
      sfx(SFX_CRUMBLE);
      g_stats.score += 50 * g_stats.depth;
      show_message("A HIDDEN VAULT!");
    }
    /* splash damage within about 1.3 cells */
    for (int i = 0; i < MAX_MONSTERS; ++i){
      struct Monster* m = &g_monsters[i];
      if (!m->alive) continue;
      if (approx_dist(m->x - p->x, m->y - p->y) < 0x14000){
        damage_monster(m, 3);
      }
    }
  } else {
    sfx(SFX_FIZZLE);
  }
}

/* Fly projectiles one tick in two half steps (so thin doors cannot be
 * skipped), hitting walls, monsters, or the player at (px, py). */
static void update_shots(int px, int py){
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Projectile* p = &g_shots[i];
    if (!p->active) continue;
    p->life -= 1;
    if (p->life <= 0){
      p->active = 0;
      continue;
    }
    /* two half steps so fast shots cannot skip through thin doors */
    for (int sub = 0; sub < 2 && p->active; ++sub){
      p->x += p->vx >> 1;
      p->y += p->vy >> 1;
      if (point_solid(p->x, p->y)){
        p->x -= p->vx >> 1;
        p->y -= p->vy >> 1;
        explode(p);
        break;
      }
      if (p->from_player){
        for (int j = 0; j < MAX_MONSTERS; ++j){
          struct Monster* m = &g_monsters[j];
          if (!m->alive) continue;
          if (approx_dist(m->x - p->x, m->y - p->y) < 0x6000){
            damage_monster(m, p->kind == SHOT_FIRE ? 5 : 2);
            explode(p);
            break;
          }
        }
      } else if (approx_dist(px - p->x, py - p->y) < 0x5000){
        player_hurt(10);
        explode(p);
      }
    }
    if (p->active){
      int radius = p->kind == SHOT_FIRE ? 0x30000 : 0x1C000;
      add_light(p->x, p->y, radius);
    }
  }
}

/* ---- Monsters ---- */

/* Straight-line visibility between two world points, sampled every 1/4 cell. */
static bool line_clear(int x0, int y0, int x1, int y1){
  int dx = x1 - x0;
  int dy = y1 - y0;
  int dist = approx_dist(dx, dy);
  int steps = (dist >> 14) + 1;
  if (steps > 64) return false;
  int sx = dx / steps;
  int sy = dy / steps;
  int x = x0;
  int y = y0;
  for (int i = 0; i < steps; ++i){
    x += sx;
    y += sy;
    int c = g_map[cell_at(x >> 16, y >> 16)];
    if (c != CELL_EMPTY){
      if (c < CELL_DOOR_BASE) return false;
      if (g_doors[c - CELL_DOOR_BASE].open < DOOR_PASSABLE) return false;
    }
  }
  return true;
}

/* Reserve cell (nx, ny) for monster `index` and start sliding into it,
 * opening a door if one is there. */
static void monster_start_move(struct Monster* m, int index, int nx, int ny){
  occupant[cell_at(m->cx, m->cy)] = 0;
  m->from_x = m->x;
  m->from_y = m->y;
  m->cx = nx;
  m->cy = ny;
  occupant[cell_at(nx, ny)] = index + 1;
  m->move_t = 0;
  m->move_len = kinds[m->kind].step_ticks;
  door_open(nx, ny);
}

/* One AI decision for an idle monster: wake when the player is near on the
 * flow field, bite when adjacent, shoot when in clear sight (ranged kinds),
 * otherwise step downhill toward the player. */
static void think(struct Monster* m, int index, int pcx, int pcy, int px, int py){
  struct MonsterKind* k = &kinds[m->kind];
  int here = flow[cell_at(m->cx, m->cy)];
  if (!m->awake){
    if (here <= k->aggro){
      m->awake = 1;
      if (m->kind == MON_WRAITH || m->kind == MON_EYE) sfx(SFX_GROWL);
    } else {
      m->cooldown = 20 + rng_range(40);
      return;
    }
  }
  /* adjacent: bite */
  int adx = m->cx - pcx;
  int ady = m->cy - pcy;
  if (adx < 0) adx = -adx;
  if (ady < 0) ady = -ady;
  if (!k->ranged && adx + ady == 1){
    m->attack_t = 16;
    m->cooldown = k->attack_ticks;
    player_hurt(k->damage);
    return;
  }
  /* the Warden winds up (charge frame) before each volley */
  if (k->ranged == 2 && m->attack_t == 0 && here <= 10 && line_clear(m->x, m->y, px, py)){
    m->attack_t = WARDEN_CHARGE_TICKS;
    m->cooldown = WARDEN_CHARGE_TICKS;
    sfx(SFX_GROWL);
    return;
  }
  /* ranged: shoot when the player is visible and not too far */
  if (k->ranged && here <= (k->ranged == 2 ? 10 : 8) && line_clear(m->x, m->y, px, py)){
    int dx = px - m->x;
    int dy = py - m->y;
    int d = approx_dist(dx, dy);
    if (d > 0){
      /* velocity = (dx, dy) / d * speed; 2^32 / d is 1/d in 16.16 */
      int speed = 0x2400;
      int inv = udiv32(0xFFFFFFFF, (unsigned)d);
      int vx = fixmul(fixmul(dx, inv), speed);
      int vy = fixmul(fixmul(dy, inv), speed);
      spawn_shot(SHOT_ENEMY, m->x, m->y, vx, vy, false);
      if (k->ranged == 2){
        /* a fan of three: the center shot plus two turned by ~14 degrees */
        int c = icos(160);
        int sn = isin(160);
        spawn_shot(SHOT_ENEMY, m->x, m->y, fixmul(vx, c) - fixmul(vy, sn), fixmul(vx, sn) + fixmul(vy, c), false);
        spawn_shot(SHOT_ENEMY, m->x, m->y, fixmul(vx, c) + fixmul(vy, sn), fixmul(vy, c) - fixmul(vx, sn), false);
      }
      sfx(SFX_ENEMY_SHOOT);
    }
    m->attack_t = k->ranged == 2 ? 0 : 20;
    m->cooldown = k->attack_ticks;
    return;
  }
  /* step downhill on the flow field */
  int best = here;
  int bx = -1;
  int by = -1;
  int start = rng_range(4);
  for (int j = 0; j < 4; ++j){
    int dir = (start + j) & 3;
    int nx = m->cx + (dir == 0) - (dir == 2);
    int ny = m->cy + (dir == 1) - (dir == 3);
    if (!in_map(nx, ny)) continue;
    int c = cell_at(nx, ny);
    if (occupant[c] || (nx == pcx && ny == pcy)) continue;
    int v = g_map[c];
    if (v != CELL_EMPTY){
      if (v < CELL_DOOR_BASE || g_doors[v - CELL_DOOR_BASE].locked) continue;
    }
    if (flow[c] < best){
      best = flow[c];
      bx = nx;
      by = ny;
    }
  }
  if (bx >= 0){
    monster_start_move(m, index, bx, by);
  }
  m->cooldown = k->think_ticks;
}

/* Animate moving monsters and let idle ones think when their cooldown ends. */
static void update_monsters(int pcx, int pcy, int px, int py){
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (!m->alive) continue;
    m->anim += 1;
    if (m->flash > 0) m->flash -= 1;
    if (m->attack_t > 0) m->attack_t -= 1;
    if (m->move_len > 0){
      m->move_t += 1;
      int tx = (m->cx << 16) + FIX_HALF;
      int ty = (m->cy << 16) + FIX_HALF;
      int k = (m->move_t << 16) / m->move_len;
      m->x = m->from_x + fixmul(tx - m->from_x, k);
      m->y = m->from_y + fixmul(ty - m->from_y, k);
      if (m->move_t >= m->move_len){
        m->move_len = 0;
        m->x = tx;
        m->y = ty;
      }
      continue;
    }
    if (m->cooldown > 0){
      m->cooldown -= 1;
      continue;
    }
    think(m, i, pcx, pcy, px, py);
  }
}

/* ---- Items ---- */

static void update_items(int pcx, int pcy){
  for (int i = 0; i < MAX_ITEMS; ++i){
    struct Item* it = &g_items[i];
    if (!it->active) continue;
    it->phase += 1;
    if (it->cx != pcx || it->cy != pcy) continue;
    int x = (it->cx << 16) + FIX_HALF;
    int y = (it->cy << 16) + FIX_HALF;
    switch (it->type){
      case ITEM_CRYSTAL:
        g_stats.crystals += 1;
        g_stats.score += 10 * g_stats.depth;
        spawn_effect(EFFECT_GEM_PUFF, x, y, 0x3000);
        spawn_burst(x, y, 0x2000, 8, 0x0CFA, 0x0A00);
        sfx(SFX_CRYSTAL);
        it->active = 0;
        break;
      case ITEM_POTION_HP:
        if (g_stats.hp >= g_stats.max_hp) break;
        g_stats.hp += 40;
        if (g_stats.hp > g_stats.max_hp) g_stats.hp = g_stats.max_hp;
        sfx(SFX_POTION);
        show_message("HEALTH RESTORED");
        it->active = 0;
        break;
      case ITEM_POTION_MP:
        if (g_stats.mp >= MAX_MP) break;
        g_stats.mp += 60;
        if (g_stats.mp > MAX_MP) g_stats.mp = MAX_MP;
        sfx(SFX_POTION);
        show_message("MANA RESTORED");
        it->active = 0;
        break;
      case ITEM_KEY:
        g_stats.keys += 1;
        sfx(SFX_KEY);
        show_message("YOU FOUND THE RUNE KEY");
        it->active = 0;
        break;
      case ITEM_PORTAL:
        g_stats.exit_reached = 1;
        break;
      case ITEM_HEART:
        g_stats.exit_reached = 2;
        it->active = 0;
        break;
      default:
        break;
    }
  }
}

/* ---- Per tick ---- */

/* Advance the world one 60 Hz tick. (px, py) is the camera position and
 * (pcx, pcy) the cell the player occupies (or is moving into). */
void world_tick(int pcx, int pcy, int px, int py, bool player_moved){
  g_light_count = 0;
  if (player_moved || map_changed){
    bfs_from(pcx, pcy, flow, false);
    map_changed = false;
  }
  for (int i = 0; i < g_door_count; ++i){
    struct Door* d = &g_doors[i];
    if (d->opening){
      d->open += 0x1800;
      if (d->open >= FIX_ONE){
        d->open = FIX_ONE;
        d->opening = 0;
      }
    }
  }
  update_monsters(pcx, pcy, px, py);
  update_shots(px, py);
  update_items(pcx, pcy);
  update_particles();
  for (int i = 0; i < MAX_EFFECTS; ++i){
    struct Effect* e = &g_effects[i];
    if (!e->active) continue;
    e->t += 1;
    if (e->t >= 16) e->active = 0;
  }
}

/* Queue billboards and lights for everything visible this frame. */
void world_draw(int frame){
  for (int i = 0; i < prop_count; ++i){
    struct Prop* p = &props[i];
    /* offset from the cell center so props do not hide items placed later */
    int x = (p->cx << 16) + 0x5000;
    int y = (p->cy << 16) + 0xB000;
    struct Billboard* b = add_billboard(x, y, 0, p->kind ? 0x6800 : 0x6000,
                                        p->kind ? SPR_MUSHROOMS : SPR_BONES);
    if (p->kind){
      if (b) b->light = 4;
      add_light(x, y, 0x14000);
    }
  }
  for (int i = 0; i < torch_count; ++i){
    struct Torch* t = &torches[i];
    int flicker = (frame + t->phase) >> 2;
    struct Billboard* b = add_billboard(t->x, t->y, 0x5000, 0x7000, SPR_TORCH + (flicker & 3));
    if (b) b->flags = BB_FULLBRIGHT;
    /* the light breathes a little: radius 2.6..3.0 cells */
    add_light(t->x, t->y, 0x29000 + ((isin(frame * 37 + t->phase * 64) + FIX_ONE) >> 4));
  }
  for (int i = 0; i < MAX_ITEMS; ++i){
    struct Item* it = &g_items[i];
    if (!it->active) continue;
    int x = (it->cx << 16) + FIX_HALF;
    int y = (it->cy << 16) + FIX_HALF;
    struct Billboard* b = NULL;
    switch (it->type){
      case ITEM_CRYSTAL:
        b = add_billboard(x, y, 0, 26000, SPR_CRYSTAL + ((it->phase >> 3) & 3));
        if (b) b->light = 2;
        add_light(x, y, 0x14000);
        break;
      case ITEM_POTION_HP:
        add_billboard(x, y, 0, 24000, SPR_POTION_HP);
        break;
      case ITEM_POTION_MP:
        add_billboard(x, y, 0, 24000, SPR_POTION_MP);
        break;
      case ITEM_KEY:
        b = add_billboard(x, y, 2000 + ((isin(it->phase * 64) * 3) >> 6), 24000, SPR_KEY);
        if (b) b->light = 3;
        break;
      case ITEM_PORTAL:
        b = add_billboard(x, y, 6000, 54000, SPR_PORTAL + ((it->phase >> 3) & 3));
        if (b) b->flags = BB_FULLBRIGHT;
        add_light(x, y, 0x28000);
        break;
      case ITEM_HEART:
        b = add_billboard(x, y, 0, 60000, SPR_CRYSTAL + ((it->phase >> 2) & 3));
        if (b) b->flags = BB_FULLBRIGHT;
        add_light(x, y, 0x30000);
        break;
      default:
        break;
    }
  }
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (!m->alive) continue;
    struct MonsterKind* k = &kinds[m->kind];
    int frame_index = (m->anim >> 3) % k->frames;
    if (m->kind == MON_EYE){
      frame_index = m->attack_t > 0 ? 4 : ((m->anim >> 6) % 8 == 7 ? 3 : (m->anim >> 5) % 3);
    } else if (m->kind == MON_WARDEN){
      frame_index = m->attack_t > 0 ? 2 : (m->anim >> 5) & 1;
    }
    int z = k->z;
    if (m->kind == MON_BAT || m->kind == MON_EYE || m->kind == MON_WRAITH || m->kind == MON_WARDEN){
      z += isin(m->anim * 40 + i * 300) >> 4;   /* hover bob */
    }
    int size = k->size;
    if (m->attack_t > 0 && !k->ranged) size += size >> 3;   /* lunge */
    struct Billboard* b = add_billboard(m->x, m->y, z, size, k->sprite + frame_index);
    if (b && m->flash > 0 && (m->flash & 2)) b->flags = BB_FLASH;
    if (b && m->kind == MON_WRAITH) b->light = 1;
    if (m->kind == MON_WARDEN){
      if (b) b->light = 2;
      add_light(m->x, m->y, m->attack_t > 0 ? 0x30000 : 0x20000);
    }
  }
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Projectile* p = &g_shots[i];
    if (!p->active) continue;
    int sprite = p->kind == SHOT_FIRE ? SPR_ORB_FIRE : (p->kind == SHOT_SPARK ? SPR_ORB_SPARK : SPR_ORB_ENEMY);
    struct Billboard* b = add_billboard(p->x, p->y, FIX_HALF - 0x2000, p->kind == SHOT_FIRE ? 22000 : 16000, sprite);
    if (b) b->flags = BB_FULLBRIGHT;
  }
  for (int i = 0; i < MAX_EFFECTS; ++i){
    struct Effect* e = &g_effects[i];
    if (!e->active) continue;
    int base = SPR_PUFF_GEM;
    if (e->kind == EFFECT_FIRE_PUFF) base = SPR_PUFF_FIRE;
    else if (e->kind == EFFECT_SPARK_PUFF) base = SPR_PUFF_SPARK;
    else if (e->kind == EFFECT_PINK_PUFF) base = SPR_PUFF_PINK;
    struct Billboard* b = add_billboard(e->x, e->y, e->z - 0x3000 - e->t * 0x200, 30000 + e->t * 1500, base + (e->t >> 2));
    if (b) b->flags = BB_FULLBRIGHT;
    if (e->kind == EFFECT_FIRE_PUFF && e->t < 8) add_light(e->x, e->y, 0x40000 - e->t * 0x6000);
  }
  (void)frame;
}

/* Remaining monsters, for the HUD and level stats. */
int world_monsters_left(void){
  return monster_count_alive();
}

/* Monster flow distance of a cell (for the automap's danger hints). */
int world_flow(int cx, int cy){
  return flow[cell_at(cx, cy)];
}

/* The awake Warden's health, for the boss bar; false when there is none. */
bool world_boss(int* hp, int* max_hp){
  for (int i = 0; i < MAX_MONSTERS; ++i){
    struct Monster* m = &g_monsters[i];
    if (m->alive && m->kind == MON_WARDEN && m->awake && flow[cell_at(m->cx, m->cy)] <= 10){
      *hp = m->hp;
      *max_hp = kinds[MON_WARDEN].hp;
      return true;
    }
  }
  return false;
}
