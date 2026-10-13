#include "skyforge.h"

/*
 * Procedural 3D canyon terrain generator, 3-phase Campaign mission progression
 * (Shield Towers -> Sector Guardian Boss -> Emerald Warp Portal), smooth
 * starfighter flight & canyon auto-guidance physics, Hover-Drone Terrain Forge
 * controls, and projectile/entity simulation.
 */

unsigned short g_terrain[16384];
struct Entity g_entities[64];
struct Shot g_shots[32];
struct Particle g_particles[64];
struct Player g_player;

int g_sector = 1;
int g_biome = BIOME_EMERALD;
int g_mode = MODE_CAMPAIGN;
int g_state = STATE_TITLE;
unsigned g_frame = 0;
unsigned g_seed = 0x5A17F04Eu;
bool g_autopilot = false;
bool g_demo_mode = false;
char g_banner_msg[40];
int g_banner_timer = 0;

static unsigned rng_state = 0x5A17F04Eu;
static unsigned canyon_seed_off = 0;

/* Seed the deterministic xorshift32 generator. */
void rng_seed(unsigned s){
  rng_state = s ? s : 0x5A17F04Eu;
}

/* Return the next 32-bit pseudo-random number. */
unsigned rng_next(void){
  unsigned x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

/* Return a pseudo-random integer in [0, bound) without software division. */
int rng_range(int bound){
  if (bound <= 1) return 0;
  unsigned r = rng_next() & 0x7FFF;
  return (int)((r * (unsigned)bound) >> 15);
}

/* Copy a bounded status banner message shown near the top of the viewport. */
void set_status_banner(char* msg, int duration_ticks){
  int i = 0;
  while (msg[i] && i < 38){
    g_banner_msg[i] = msg[i];
    i += 1;
  }
  g_banner_msg[i] = 0;
  g_banner_timer = duration_ticks;
}

/* Sample the terrain height (0..63) at cell (cx, cy) with 128x128 wrapping. */
int terrain_height_cell(int cx, int cy){
  int idx = ((cy & MAP_MASK) << MAP_SHIFT) | (cx & MAP_MASK);
  return g_terrain[idx] & 0x3F;
}

/* Sample the 16.16 terrain altitude at 16.16 world position (x, y). */
int terrain_height_at(int x, int y){
  int cx = (x >> FIX_SHIFT) & MAP_MASK;
  int cy = (y >> FIX_SHIFT) & MAP_MASK;
  return terrain_height_cell(cx, cy) << FIX_SHIFT;
}

/* Return the canyon center X coordinate (0..127) at map row `cy`. */
int canyon_center_at(int cy){
  int a1 = ((cy << 2) + (int)(canyon_seed_off & 0x3FF)) & ANGLE_MASK;
  int a2 = ((cy << 3) + (int)((canyon_seed_off >> 5) & 0x3FF)) & ANGLE_MASK;
  int sway = (isin(a1) * 10 + icos(a2) * 4) >> FIX_SHIFT;
  return (64 + sway) & MAP_MASK;
}

/* Recompute directional sun slope and biome color indices over [x0..x1, y0..y1]. */
void terrain_relight_rect(int x0, int y0, int x1, int y1){
  for (int cy = y0; cy <= y1; ++cy){
    int wy = cy & MAP_MASK;
    int row_base = wy << MAP_SHIFT;
    for (int cx = x0; cx <= x1; ++cx){
      int wx = cx & MAP_MASK;
      int idx = row_base | wx;
      unsigned short cur = g_terrain[idx];
      int h = cur & 0x3F;
      int old_light = (cur >> 14) & 3;
      int light = 1;
      if (old_light == 3 && h > WATER_LEVEL){
        light = 3;
      } else if (h <= WATER_LEVEL){
        light = 1;
      } else {
        int h_east = g_terrain[row_base | ((wx + 1) & MAP_MASK)] & 0x3F;
        int h_west = g_terrain[row_base | ((wx - 1) & MAP_MASK)] & 0x3F;
        int h_south = g_terrain[((wy + 1) & MAP_MASK) << MAP_SHIFT | wx] & 0x3F;
        int slope = (h_west - h_east) + ((h - h_south) >> 1);
        if (slope >= 2) light = 2;
        else if (slope <= -2) light = 0;
        else light = 1;
      }
      int col_idx = (light << 6) | h;
      g_terrain[idx] = (unsigned short)(h | (col_idx << 8));
    }
  }
}

/* Carve a crater or raise a peak centered at cell (cx, cy) with radius `r`. */
void terrain_deform(int cx, int cy, int radius, int delta_h, int paint_kind){
  if (radius < 1) radius = 1;
  if (radius > 12) radius = 12;
  int r2 = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy){
    for (int dx = -radius; dx <= radius; ++dx){
      int d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      int wx = (cx + dx) & MAP_MASK;
      int wy = (cy + dy) & MAP_MASK;
      int idx = (wy << MAP_SHIFT) | wx;
      int h = g_terrain[idx] & 0x3F;
      int falloff = radius - approx_dist2d(dx, dy);
      if (falloff < 1) falloff = 1;
      int dh = (delta_h * falloff) / radius;
      h += dh;
      if (h < WATER_LEVEL - 2) h = WATER_LEVEL - 2;
      if (h > MAX_HEIGHT) h = MAX_HEIGHT;
      int light = (g_terrain[idx] >> 14) & 3;
      if (paint_kind == 1){
        if (d2 >= (r2 >> 1) && h > WATER_LEVEL) light = 3;
      } else if (paint_kind == 2){
        light = 3;
      } else if (paint_kind == 3){
        h = WATER_LEVEL;
        light = 2;
      }
      g_terrain[idx] = (unsigned short)(h | (((light << 6) | h) << 8));
    }
  }
  terrain_relight_rect(cx - radius - 1, cy - radius - 1,
                       cx + radius + 1, cy + radius + 1);
}

/* Apply a Terrain Forge sculptor brush at cell (cx, cy). */
void forge_apply_tool(int cx, int cy, int tool, int radius, bool inverse){
  if (tool == TOOL_RAISE){
    terrain_deform(cx, cy, radius, inverse ? -4 : 4, 0);
  } else if (tool == TOOL_LOWER){
    terrain_deform(cx, cy, radius, inverse ? 4 : -4, 0);
  } else if (tool == TOOL_SMOOTH){
    int sum = 0;
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy){
      for (int dx = -radius; dx <= radius; ++dx){
        sum += terrain_height_cell(cx + dx, cy + dy);
        count += 1;
      }
    }
    int avg = count > 0 ? (sum / count) : 16;
    for (int dy = -radius; dy <= radius; ++dy){
      for (int dx = -radius; dx <= radius; ++dx){
        if (dx * dx + dy * dy > radius * radius) continue;
        int wx = (cx + dx) & MAP_MASK;
        int wy = (cy + dy) & MAP_MASK;
        int idx = (wy << MAP_SHIFT) | wx;
        int h = g_terrain[idx] & 0x3F;
        h = (h + avg) >> 1;
        g_terrain[idx] = (unsigned short)(h | ((1 << 6 | h) << 8));
      }
    }
    terrain_relight_rect(cx - radius - 1, cy - radius - 1,
                         cx + radius + 1, cy + radius + 1);
  } else if (tool == TOOL_CRATER){
    terrain_deform(cx, cy, radius + 2, 5, 1);
    terrain_deform(cx, cy, radius - 1, -10, 3);
  } else if (tool == TOOL_CRYSTAL){
    terrain_deform(cx, cy, radius, inverse ? 0 : 2, 2);
  } else if (tool == TOOL_LAVA){
    terrain_deform(cx, cy, radius, -8, 3);
  } else if (tool == TOOL_RING){
    for (int i = 0; i < MAX_ENTITIES; ++i){
      if (!g_entities[i].active){
        struct Entity* e = &g_entities[i];
        e->active = 1;
        e->kind = inverse ? ENT_ARCH : ENT_RING;
        e->x = ((cx & MAP_MASK) << FIX_SHIFT) + FIX_HALF;
        e->y = ((cy & MAP_MASK) << FIX_SHIFT) + FIX_HALF;
        int th = terrain_height_cell(cx, cy);
        if (th < WATER_LEVEL) th = WATER_LEVEL;
        e->z = (th + 9) << FIX_SHIFT;
        e->hp = 20;
        e->max_hp = 20;
        e->phase = i;
        break;
      }
    }
  }
}

/* Spawn one particle into the first free slot. */
static void spawn_particle(int x, int y, int z, int vx, int vy, int vz,
                           int life, unsigned short color){
  for (int i = 0; i < MAX_PARTICLES; ++i){
    if (!g_particles[i].active){
      g_particles[i].active = 1;
      g_particles[i].x = x;
      g_particles[i].y = y;
      g_particles[i].z = z;
      g_particles[i].vx = vx;
      g_particles[i].vy = vy;
      g_particles[i].vz = vz;
      g_particles[i].life = life;
      g_particles[i].color = color;
      return;
    }
  }
}

/* Spawn an explosion burst of `count` 3D particles and an explosion billboard. */
static void spawn_burst(int x, int y, int z, int count, unsigned short c1, unsigned short c2){
  for (int i = 0; i < count; ++i){
    int ang = rng_range(ANGLE_COUNT);
    int spd = (rng_range(12) + 4) << 11;
    int vx = fixmul(icos(ang), spd);
    int vy = fixmul(isin(ang), spd);
    int vz = (rng_range(16) - 6) << 12;
    spawn_particle(x, y, z, vx, vy, vz, 14 + rng_range(14), (i & 1) ? c1 : c2);
  }
  if (count >= 10){
    for (int i = 0; i < MAX_ENTITIES; ++i){
      if (!g_entities[i].active){
        struct Entity* ex = &g_entities[i];
        ex->active = 1;
        ex->kind = ENT_EXPLOSION;
        ex->x = x;
        ex->y = y;
        ex->z = z;
        ex->hp = 16;
        ex->max_hp = 16;
        ex->phase = 0;
        break;
      }
    }
  }
}

/* Spawn a projectile into the first free shot slot. */
static void spawn_shot(int kind, int x, int y, int z, int vx, int vy, int vz,
                       int life, int color){
  for (int i = 0; i < MAX_SHOTS; ++i){
    if (!g_shots[i].active){
      g_shots[i].active = 1;
      g_shots[i].kind = kind;
      g_shots[i].x = x;
      g_shots[i].y = y;
      g_shots[i].z = z;
      g_shots[i].vx = vx;
      g_shots[i].vy = vy;
      g_shots[i].vz = vz;
      g_shots[i].life = life;
      g_shots[i].color = color;
      return;
    }
  }
}

/* Count remaining hostile targets or Shield Towers in Campaign mode. */
int world_enemies_remaining(void){
  int count = 0;
  for (int i = 0; i < MAX_ENTITIES; ++i){
    if (!g_entities[i].active) continue;
    int k = g_entities[i].kind;
    if (k == ENT_GENERATOR || k == ENT_SCOUT || k == ENT_INTERCEPTOR ||
        k == ENT_TURRET || k == ENT_STEALTH || k == ENT_BOSS){
      count += 1;
    }
  }
  return count;
}

/* Generate a 128x128 3D canyon world and structured mission objectives. */
void world_generate(int sector, int mode, unsigned seed){
  if (sector < 1) sector = 1;
  if (sector > MAX_SECTORS) sector = MAX_SECTORS;
  g_sector = sector;
  g_mode = mode;
  g_biome = (sector - 1) % BIOME_COUNT;
  rng_seed(seed ^ (unsigned)(sector * 0x9E3779B9u));

  for (int i = 0; i < MAX_ENTITIES; ++i) g_entities[i].active = 0;
  for (int i = 0; i < MAX_SHOTS; ++i) g_shots[i].active = 0;
  for (int i = 0; i < MAX_PARTICLES; ++i) g_particles[i].active = 0;

  canyon_seed_off = rng_next();

  /* Synthesize dramatic 3D canyon valley with river channel and mountain walls */
  for (int cy = 0; cy < MAP_SIZE; ++cy){
    int riv_x = canyon_center_at(cy);
    for (int cx = 0; cx < MAP_SIZE; ++cx){
      int dx = cx - riv_x;
      if (dx > 64) dx -= 128;
      if (dx < -64) dx += 128;
      int adx = dx < 0 ? -dx : dx;

      int ax = ((cx << 4) + (int)(canyon_seed_off & 0x1FF)) & ANGLE_MASK;
      int ay = ((cy << 4) + (int)((canyon_seed_off >> 4) & 0x1FF)) & ANGLE_MASK;
      int axy = (((cx + cy) << 3) + (int)((canyon_seed_off >> 8) & 0x1FF)) & ANGLE_MASK;
      int ridge = ((isin(ax) + icos(ay)) * 9 + isin(axy) * 6) >> FIX_SHIFT;

      int h = WATER_LEVEL;
      if (adx <= 9){
        /* Wide navigable canyon river channel */
        h = WATER_LEVEL - 2 + (adx >> 2);
      } else if (adx <= 14){
        /* Golden sandy/rocky shoreline and low terraces */
        h = WATER_LEVEL + (adx - 9);
      } else if (adx <= 25){
        /* Steep canyon walls rising on both sides of the corridor */
        h = WATER_LEVEL + 5 + ((adx - 14) * 2) + (ridge >> 1);
      } else {
        /* High alpine mountain peaks and mesas */
        h = 30 + ridge + ((adx - 25) >> 1);
      }
      if (h < 2) h = 2;
      if (h > MAX_HEIGHT) h = MAX_HEIGHT;

      int light = 1;
      if (h >= 20 && h <= 40 && ((cx ^ cy) & 15) == 0){
        light = 3;
      }
      g_terrain[(cy << MAP_SHIFT) | cx] = (unsigned short)(h | (((light << 6) | h) << 8));
    }
  }
  terrain_relight_rect(0, 0, MAP_SIZE - 1, MAP_SIZE - 1);

  int start_cy = 112;
  int start_cx = canyon_center_at(start_cy);
  bool fresh_run = (sector == 1) || (g_player.hp <= 0);
  int prev_score = fresh_run ? 0 : g_player.score;
  int prev_kills = fresh_run ? 0 : g_player.kills;
  int prev_wpn = fresh_run ? 1 : g_player.weapon_level;
  int prev_best_race = g_player.best_race_ticks;

  g_player.x = (start_cx << FIX_SHIFT) + FIX_HALF;
  g_player.y = (start_cy << FIX_SHIFT) + FIX_HALF;
  g_player.z = (WATER_LEVEL + 14) << FIX_SHIFT;
  g_player.angle = 768; /* Facing north (-Y) down the canyon */
  g_player.turn_vel = 0;
  g_player.pitch = 0;
  g_player.roll = 0;
  g_player.barrel_spin = 0;
  g_player.barrel_cooldown = 0;
  g_player.speed = (mode == MODE_FORGE) ? 0 : 0x3800;
  g_player.max_hp = 100;
  g_player.hp = 100;
  g_player.energy = 100;
  g_player.weapon_level = prev_wpn;
  g_player.bombs = 3 + (sector >> 1);
  g_player.fire_cooldown = 0;
  g_player.bomb_cooldown = 0;
  g_player.hurt_flash = 0;
  g_player.boost_active = 0;
  g_player.shake = 0;
  g_player.score = prev_score;
  g_player.combo = 1;
  g_player.combo_timer = 0;
  g_player.kills = prev_kills;
  g_player.rings_passed = 0;
  g_player.towers_left = 3;
  g_player.sector_phase = PHASE_TOWERS;
  g_player.next_gate = 0;
  g_player.lap = 1;
  g_player.race_ticks = 0;
  g_player.best_race_ticks = prev_best_race;
  g_player.aim_x = 0;
  g_player.aim_y = 0;
  g_player.lock_target = -1;
  g_player.nav_target = -1;
  g_player.nav_bearing = 0;
  g_player.forge_tool = TOOL_RAISE;
  g_player.forge_radius = 4;
  g_player.forge_cursor_x = start_cx;
  g_player.forge_cursor_y = (start_cy - 10) & MAP_MASK;

  int ent_idx = 0;

  if (mode == MODE_RACE){
    for (int g = 0; g < RACE_GATE_COUNT && ent_idx < MAX_ENTITIES; ++g){
      int cy = (start_cy - 5 - g * 5) & MAP_MASK;
      int cx = canyon_center_at(cy);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_GATE;
      e->x = (cx << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      e->z = (WATER_LEVEL + 9 + ((g & 1) << 1)) << FIX_SHIFT;
      e->phase = g;
      e->hp = 100;
      e->max_hp = 100;
    }
    for (int a = 0; a < 6 && ent_idx < MAX_ENTITIES; ++a){
      int cy = (start_cy - 12 - a * 20) & MAP_MASK;
      int cx = canyon_center_at(cy);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_ARCH;
      e->x = (cx << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      e->z = (WATER_LEVEL + 13) << FIX_SHIFT;
      e->hp = 200;
      e->max_hp = 200;
    }
    for (int r = 0; r < 3 && ent_idx < MAX_ENTITIES; ++r){
      int cy = (start_cy - 6 - r * 12) & MAP_MASK;
      int cx = canyon_center_at(cy);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_RIVAL;
      e->x = ((cx + (r - 1) * 2) << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      e->z = (WATER_LEVEL + 11) << FIX_SHIFT;
      e->angle = 768;
      e->phase = r;
      e->hp = 50;
      e->max_hp = 50;
    }
    set_status_banner("FLY THROUGH ALL 24 NEON GATES!", 150);
    return;
  }

  /* Spawn 3 Armored Canyon Shield Towers (Phase 0 primary objectives!) */
  for (int t = 0; t < 3 && ent_idx < MAX_ENTITIES; ++t){
    int cy = (start_cy - 16 - t * 18) & MAP_MASK;
    int side = (t == 1) ? -3 : (t == 2 ? 3 : 0);
    int cx = (canyon_center_at(cy) + side) & MAP_MASK;
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    e->kind = ENT_GENERATOR;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    e->z = (WATER_LEVEL + 8) << FIX_SHIFT;
    e->hp = 20 + sector * 4;
    e->max_hp = e->hp;
    e->cooldown = 40 + t * 15;
    e->phase = t;
  }

  /* Spawn Golden Power Rings and Stone Arches along the canyon river */
  for (int r = 0; r < 9 && ent_idx < MAX_ENTITIES; ++r){
    int cy = (start_cy - 8 - r * 11) & MAP_MASK;
    int cx = canyon_center_at(cy);
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    e->kind = (r % 3 == 2) ? ENT_ARCH : ENT_RING;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    e->z = (WATER_LEVEL + (e->kind == ENT_ARCH ? 13 : 9)) << FIX_SHIFT;
    e->hp = 50;
    e->max_hp = 50;
    e->phase = r;
  }

  if (mode == MODE_FORGE){
    set_status_banner("HOVER DRONE: WASD MOVE, LMB SCULPT!", 150);
    return;
  }

  /* Spawn hostile fighter squadrons guarding the canyon */
  int num_enemies = 6 + sector * 2;
  for (int m = 0; m < num_enemies && ent_idx < MAX_ENTITIES - 2; ++m){
    int cy = (start_cy - 12 - m * 8) & MAP_MASK;
    int cx = (canyon_center_at(cy) + ((m & 1) ? 4 : -4)) & MAP_MASK;
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    if ((m & 3) == 0){
      e->kind = ENT_TURRET;
      e->z = (WATER_LEVEL + 6) << FIX_SHIFT;
      e->hp = 14 + sector * 3;
    } else if ((m & 3) == 1 && sector >= 2){
      e->kind = ENT_INTERCEPTOR;
      e->z = (WATER_LEVEL + 11) << FIX_SHIFT;
      e->hp = 12 + sector * 3;
    } else if ((m & 3) == 2 && sector >= 3){
      e->kind = ENT_STEALTH;
      e->z = (WATER_LEVEL + 13) << FIX_SHIFT;
      e->hp = 14 + sector * 3;
    } else {
      e->kind = ENT_SCOUT;
      e->z = (WATER_LEVEL + 10) << FIX_SHIFT;
      e->hp = 10 + sector * 2;
    }
    e->max_hp = e->hp;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    e->angle = 256;
    e->cooldown = 35 + (m << 3);
    e->phase = m;
  }

  /* Sector 5 also has the Leviathan Boss ready in Phase 1 (or immediately for test) */
  if (sector == MAX_SECTORS && ent_idx < MAX_ENTITIES){
    int cy = (start_cy - 32) & MAP_MASK;
    int cx = canyon_center_at(cy);
    struct Entity* boss = &g_entities[ent_idx++];
    boss->active = 1;
    boss->kind = ENT_BOSS;
    boss->x = (cx << FIX_SHIFT) + FIX_HALF;
    boss->y = (cy << FIX_SHIFT) + FIX_HALF;
    boss->z = (WATER_LEVEL + 16) << FIX_SHIFT;
    boss->hp = 160;
    boss->max_hp = 160;
    boss->cooldown = 40;
    boss->shield_gen = 0x0F;
    g_player.sector_phase = PHASE_GUARDIAN;
    g_player.towers_left = 0;
    sfx_trigger(SFX_BOSS_ALARM);
    set_status_banner("FINAL BOSS: LEVIATHAN DREADNOUGHT!", 180);
  } else {
    set_status_banner("PHASE 1: DESTROY 3 SHIELD TOWERS!", 150);
  }
}

/* Wrap a 16.16 signed delta into [-64*FIX_ONE, +64*FIX_ONE) on the 128x128 torus. */
static int wrap_delta(int d){
  int half_map = 64 << FIX_SHIFT;
  int full_map = 128 << FIX_SHIFT;
  while (d > half_map) d -= full_map;
  while (d < -half_map) d += full_map;
  return d;
}

/* Spawn the Sector Guardian Boss (when all 3 Shield Towers fall) or Warp Portal. */
static void advance_campaign_phase(void){
  int dir_y = isin(g_player.angle);
  int full_map_mask = (MAP_SIZE << FIX_SHIFT) - 1;

  if (g_player.sector_phase == PHASE_TOWERS){
    int towers = 0;
    for (int i = 0; i < MAX_ENTITIES; ++i){
      if (g_entities[i].active && g_entities[i].kind == ENT_GENERATOR){
        towers += 1;
      }
    }
    g_player.towers_left = towers;
    if (towers == 0){
      g_player.sector_phase = PHASE_GUARDIAN;
      /* Spawn Sector Guardian directly ahead in the canyon! */
      for (int i = 0; i < MAX_ENTITIES; ++i){
        if (!g_entities[i].active){
          struct Entity* boss = &g_entities[i];
          boss->active = 1;
          boss->kind = ENT_BOSS;
          int by = ((g_player.y + dir_y * 22) >> FIX_SHIFT) & MAP_MASK;
          int bx = canyon_center_at(by);
          boss->x = (bx << FIX_SHIFT) + FIX_HALF;
          boss->y = (by << FIX_SHIFT) + FIX_HALF;
          boss->z = (WATER_LEVEL + 15) << FIX_SHIFT;
          boss->hp = 60 + g_sector * 25;
          boss->max_hp = boss->hp;
          boss->cooldown = 35;
          boss->shield_gen = (g_sector >= 3) ? 0x03 : 0x00;
          break;
        }
      }
      sfx_trigger(SFX_BOSS_ALARM);
      set_status_banner("SHIELDS DOWN! GUARDIAN INBOUND!", 160);
    }
  } else if (g_player.sector_phase == PHASE_GUARDIAN){
    bool boss_alive = false;
    for (int i = 0; i < MAX_ENTITIES; ++i){
      if (g_entities[i].active && g_entities[i].kind == ENT_BOSS){
        boss_alive = true;
        break;
      }
    }
    if (!boss_alive){
      g_player.sector_phase = PHASE_PORTAL;
      /* Open the Emerald Dioptase Warp Portal 14 cells ahead! */
      for (int i = 0; i < MAX_ENTITIES; ++i){
        if (!g_entities[i].active){
          struct Entity* p = &g_entities[i];
          p->active = 1;
          p->kind = ENT_PORTAL;
          int py = ((g_player.y + dir_y * 14) >> FIX_SHIFT) & MAP_MASK;
          int px = canyon_center_at(py);
          p->x = ((px << FIX_SHIFT) + FIX_HALF) & full_map_mask;
          p->y = ((py << FIX_SHIFT) + FIX_HALF) & full_map_mask;
          p->z = (WATER_LEVEL + 11) << FIX_SHIFT;
          p->hp = 100;
          p->max_hp = 100;
          break;
        }
      }
      sfx_trigger(SFX_SECTOR_CLEAR);
      set_status_banner("GUARDIAN DEFEATED! ENTER PORTAL!", 180);
    }
  }
}

/* Advance player flight/drone, weapons, mission phases, entities, and collisions. */
void world_tick(struct FlightControls* ctrl){
  g_frame += 1;
  if (g_banner_timer > 0) g_banner_timer -= 1;
  if (g_player.hurt_flash > 0) g_player.hurt_flash -= 1;
  if (g_player.shake > 0) g_player.shake -= 1;
  if (g_player.fire_cooldown > 0) g_player.fire_cooldown -= 1;
  if (g_player.bomb_cooldown > 0) g_player.bomb_cooldown -= 1;
  if (g_player.barrel_cooldown > 0) g_player.barrel_cooldown -= 1;
  if (g_player.combo_timer > 0){
    g_player.combo_timer -= 1;
    if (g_player.combo_timer == 0) g_player.combo = 1;
  }

  int full_map_mask = (MAP_SIZE << FIX_SHIFT) - 1;
  int water_z = WATER_LEVEL << FIX_SHIFT;

  /* ---- TERRAIN FORGE HOVER-DRONE MODE ---- */
  if (g_mode == MODE_FORGE && g_state == STATE_PLAY && !g_autopilot){
    g_player.angle = (g_player.angle + ctrl->turn) & ANGLE_MASK;
    g_player.roll = 0;
    int dir_x = icos(g_player.angle);
    int dir_y = isin(g_player.angle);
    int right_x = -dir_y;
    int right_y = dir_x;

    int move_spd = 0x5000;
    if (ctrl->throttle != 0){
      g_player.x = (g_player.x + fixmul(dir_x, ctrl->throttle * move_spd)) & full_map_mask;
      g_player.y = (g_player.y + fixmul(dir_y, ctrl->throttle * move_spd)) & full_map_mask;
    }
    if (ctrl->strafe != 0){
      g_player.x = (g_player.x + fixmul(right_x, ctrl->strafe * move_spd)) & full_map_mask;
      g_player.y = (g_player.y + fixmul(right_y, ctrl->strafe * move_spd)) & full_map_mask;
    }
    g_player.z += ctrl->pitch << 13;
    int min_drone_z = (WATER_LEVEL + 16) << FIX_SHIFT;
    int max_drone_z = (MAX_HEIGHT + 22) << FIX_SHIFT;
    if (g_player.z < min_drone_z) g_player.z = min_drone_z;
    if (g_player.z > max_drone_z) g_player.z = max_drone_z;
    g_player.pitch = -10; /* Slight downward look angle to inspect terrain */

    /* Project 3D sculpting brush onto terrain ahead of hover-drone */
    int look_dist = (14 - (g_player.aim_y >> 3)) << FIX_SHIFT;
    if (look_dist < (5 << FIX_SHIFT)) look_dist = 5 << FIX_SHIFT;
    int cur_wx = g_player.x + fixmul(dir_x, look_dist) +
                 fixmul(right_x, g_player.aim_x << 10);
    int cur_wy = g_player.y + fixmul(dir_y, look_dist) +
                 fixmul(right_y, g_player.aim_x << 10);
    g_player.forge_cursor_x = (cur_wx >> FIX_SHIFT) & MAP_MASK;
    g_player.forge_cursor_y = (cur_wy >> FIX_SHIFT) & MAP_MASK;

    if ((ctrl->sculpt_apply || ctrl->sculpt_erase) && (g_frame & 2) == 0){
      forge_apply_tool(g_player.forge_cursor_x, g_player.forge_cursor_y,
                       g_player.forge_tool, g_player.forge_radius,
                       ctrl->sculpt_erase != 0);
      sfx_trigger(SFX_SCULPT_HUM);
    }
    return;
  }

  /* ---- SMOOTH STARFIGHTER FLIGHT MODEL ---- */
  if (ctrl->barrel_roll && g_player.barrel_spin == 0 &&
      g_player.barrel_cooldown == 0 && g_player.energy >= 20){
    g_player.barrel_spin = ANGLE_COUNT;
    g_player.barrel_cooldown = 36;
    g_player.energy -= 20;
    sfx_trigger(SFX_BARREL_ROLL);
  }
  if (g_player.barrel_spin > 0){
    g_player.barrel_spin -= 64;
    if (g_player.barrel_spin < 0) g_player.barrel_spin = 0;
  }

  /* Smooth angular velocity with acceleration and damping */
  int desired_turn = ctrl->turn;
  if (g_player.turn_vel < desired_turn) g_player.turn_vel += 2;
  else if (g_player.turn_vel > desired_turn) g_player.turn_vel -= 2;

  /* Canyon Corridor Assist: gently guide ship back toward the canyon river
   * if it banks too far into the high outer mountains! */
  int player_cy = (g_player.y >> FIX_SHIFT) & MAP_MASK;
  int player_cx = (g_player.x >> FIX_SHIFT) & MAP_MASK;
  int canyon_cx = canyon_center_at((player_cy - 8) & MAP_MASK);
  int off_canyon = player_cx - canyon_cx;
  if (off_canyon > 64) off_canyon -= 128;
  if (off_canyon < -64) off_canyon += 128;
  if (off_canyon > 15){
    g_player.x -= 0x1800;
  } else if (off_canyon < -15){
    g_player.x += 0x1800;
  }

  g_player.angle = (g_player.angle + (g_player.turn_vel >> 1)) & ANGLE_MASK;

  int target_roll = g_player.turn_vel * 2;
  if (target_roll > 26) target_roll = 26;
  if (target_roll < -26) target_roll = -26;
  if (g_player.roll < target_roll) g_player.roll += 2;
  else if (g_player.roll > target_roll) g_player.roll -= 2;

  int target_pitch = ctrl->pitch * 2;
  if (target_pitch > 28) target_pitch = 28;
  if (target_pitch < -28) target_pitch = -28;
  if (g_player.pitch < target_pitch) g_player.pitch += 2;
  else if (g_player.pitch > target_pitch) g_player.pitch -= 2;

  /* Throttle and Afterburner Boost */
  int target_speed = 0x3C00;
  g_player.boost_active = 0;
  if (ctrl->throttle > 0 && g_player.energy > 2){
    target_speed = 0x6800;
    g_player.boost_active = 1;
    if ((g_frame & 1) == 0 && !g_demo_mode) g_player.energy -= 1;
  } else if (ctrl->throttle < 0){
    target_speed = 0x2000;
    if (g_player.energy < 100 && (g_frame & 1) == 0) g_player.energy += 1;
  } else {
    if (g_player.energy < 100 && (g_frame & 1) == 0) g_player.energy += 1;
  }
  if (g_player.speed < target_speed) g_player.speed += 0x0400;
  else if (g_player.speed > target_speed) g_player.speed -= 0x0400;

  int dir_x = icos(g_player.angle);
  int dir_y = isin(g_player.angle);
  int right_x = -dir_y;
  int right_y = dir_x;

  g_player.x = (g_player.x + fixmul(dir_x, g_player.speed)) & full_map_mask;
  g_player.y = (g_player.y + fixmul(dir_y, g_player.speed)) & full_map_mask;
  g_player.z += (g_player.pitch << 11) - (g_player.aim_y << 9);

  /* Cushion altitude above canyon floor and gently auto-level toward cruise altitude */
  int ground_z = terrain_height_at(g_player.x, g_player.y);
  int base_z = (ground_z > water_z) ? ground_z : water_z;
  int min_z = base_z + (6 << FIX_SHIFT);
  int cruise_z = base_z + (11 << FIX_SHIFT);
  int max_z = (MAX_HEIGHT + 18) << FIX_SHIFT;

  if (ctrl->pitch == 0 && g_player.aim_y > -12 && g_player.aim_y < 12){
    if (g_player.z < cruise_z - 0x2000) g_player.z += 0x1800;
    else if (g_player.z > cruise_z + 0x4000) g_player.z -= 0x1800;
  }
  if (g_player.z < min_z){
    g_player.z = min_z;
    if (ground_z > water_z + (8 << FIX_SHIFT) && g_player.hurt_flash == 0 && !g_demo_mode){
      g_player.hp -= 4;
      g_player.hurt_flash = 12;
      g_player.shake = 6;
      sfx_trigger(SFX_PLAYER_HURT);
    }
  }
  if (g_player.z > max_z) g_player.z = max_z;

  /* Update 3D cursor cell for AI Terrain Forge demo */
  g_player.forge_cursor_x = ((g_player.x + (dir_x << 3)) >> FIX_SHIFT) & MAP_MASK;
  g_player.forge_cursor_y = ((g_player.y + (dir_y << 3)) >> FIX_SHIFT) & MAP_MASK;
  if (g_mode == MODE_FORGE && ctrl->sculpt_apply && (g_frame & 3) == 0){
    forge_apply_tool(g_player.forge_cursor_x, g_player.forge_cursor_y,
                     g_player.forge_tool, g_player.forge_radius, false);
  }

  /* ---- ACQUIRE LOCK-ON TARGET & PRIMARY NAVIGATION WAYPOINT ---- */
  g_player.lock_target = -1;
  g_player.nav_target = -1;
  int best_lock_dist = 44 << FIX_SHIFT;
  int best_nav_dist = 0x7FFFFFFF;

  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    int dx = wrap_delta(e->x - g_player.x);
    int dy = wrap_delta(e->y - g_player.y);
    int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    int side = fixmul(dx, right_x) + fixmul(dy, right_y);
    int abs_side = side < 0 ? -side : side;
    int dist = approx_dist2d(dx >> FIX_SHIFT, dy >> FIX_SHIFT);

    /* Primary mission waypoint selection */
    bool is_objective = false;
    if (g_mode == MODE_RACE){
      is_objective = (e->kind == ENT_GATE && e->phase == g_player.next_gate);
    } else if (g_player.sector_phase == PHASE_TOWERS){
      is_objective = (e->kind == ENT_GENERATOR);
    } else if (g_player.sector_phase == PHASE_GUARDIAN){
      is_objective = (e->kind == ENT_BOSS);
    } else if (g_player.sector_phase == PHASE_PORTAL){
      is_objective = (e->kind == ENT_PORTAL);
    }
    if (is_objective && dist < best_nav_dist){
      best_nav_dist = dist;
      g_player.nav_target = i;
      /* Approximate bearing offset in screen columns */
      g_player.nav_bearing = (fwd > 0) ? (side >> 14) : (side >= 0 ? 120 : -120);
    }

    /* Weapon lock-on selection (generous cone in front of player) */
    if (e->kind == ENT_GENERATOR || (e->kind >= ENT_SCOUT && e->kind <= ENT_BOSS)){
      if (fwd > (2 << FIX_SHIFT) && fwd < best_lock_dist && abs_side < (fwd >> 1) + (3 << FIX_SHIFT)){
        best_lock_dist = fwd;
        g_player.lock_target = i;
      }
    }
  }

  /* ---- FIRE TWIN / QUAD / HYPER-SPREAD PLASMA CANNONS ---- */
  if (ctrl->fire_primary && g_player.fire_cooldown == 0){
    g_player.fire_cooldown = (g_player.weapon_level >= 3) ? 5 : 6;
    int shot_spd = g_player.speed + 0xB800;
    int aim_ang = (g_player.angle + (g_player.aim_x >> 2)) & ANGLE_MASK;
    int sx = icos(aim_ang);
    int sy = isin(aim_ang);
    int vx = fixmul(sx, shot_spd);
    int vy = fixmul(sy, shot_spd);
    int vz = (g_player.pitch - (g_player.aim_y >> 1)) << 11;

    if (g_player.lock_target >= 0){
      struct Entity* t = &g_entities[g_player.lock_target];
      int dx = wrap_delta(t->x - g_player.x);
      int dy = wrap_delta(t->y - g_player.y);
      int dist = approx_dist2d(dx >> 8, dy >> 8);
      if (dist > 4){
        int ticks = (dist << 8) / (shot_spd >> 8);
        if (ticks < 1) ticks = 1;
        vx = dx / ticks;
        vy = dy / ticks;
        vz = (t->z - g_player.z) / ticks;
      }
    }

    spawn_shot(SHOT_PLAYER_PLASMA,
               g_player.x + (right_x >> 1), g_player.y + (right_y >> 1),
               g_player.z - (1 << FIX_SHIFT), vx, vy, vz, 38, 0x0FF4);
    spawn_shot(SHOT_PLAYER_PLASMA,
               g_player.x - (right_x >> 1), g_player.y - (right_y >> 1),
               g_player.z - (1 << FIX_SHIFT), vx, vy, vz, 38, 0x0FF4);
    if (g_player.weapon_level >= 2){
      spawn_shot(SHOT_PLAYER_PLASMA,
                 g_player.x, g_player.y, g_player.z,
                 vx + (right_x >> 2), vy + (right_y >> 2), vz, 38, 0x04FF);
      spawn_shot(SHOT_PLAYER_PLASMA,
                 g_player.x, g_player.y, g_player.z,
                 vx - (right_x >> 2), vy - (right_y >> 2), vz, 38, 0x04FF);
    }
    sfx_trigger(SFX_PLASMA);
  }

  /* ---- LAUNCH SEISMIC NOVA TORPEDO ---- */
  if (ctrl->fire_seismic && g_player.bomb_cooldown == 0 && g_player.bombs > 0){
    g_player.bomb_cooldown = 32;
    if (!g_demo_mode) g_player.bombs -= 1;
    int shot_spd = g_player.speed + 0x8000;
    int vx = fixmul(dir_x, shot_spd);
    int vy = fixmul(dir_y, shot_spd);
    int vz = -0x3000 + (g_player.pitch << 10);
    spawn_shot(SHOT_PLAYER_SEISMIC,
               g_player.x + dir_x, g_player.y + dir_y,
               g_player.z - (1 << FIX_SHIFT), vx, vy, vz, 52, 0x02AF);
    sfx_trigger(SFX_SEISMIC_LAUNCH);
  }

  /* ---- UPDATE SHOTS & COLLISIONS ---- */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    s->x = (s->x + s->vx) & full_map_mask;
    s->y = (s->y + s->vy) & full_map_mask;
    s->z += s->vz;
    if (s->kind == SHOT_PLAYER_SEISMIC){
      s->vz -= 0x0260;
      if ((g_frame & 1) == 0){
        spawn_particle(s->x, s->y, s->z, 0, 0, 0x1000, 8, 0x04CF);
      }
    }
    s->life -= 1;
    int gz = terrain_height_at(s->x, s->y);
    if (gz < water_z) gz = water_z;

    bool detonate_seismic = false;
    if (s->z <= gz){
      if (s->kind == SHOT_PLAYER_SEISMIC){
        detonate_seismic = true;
      } else {
        spawn_particle(s->x, s->y, gz + FIX_ONE, 0, 0, 0x2000, 6, (unsigned short)s->color);
        s->active = 0;
        continue;
      }
    }

    if (s->kind == SHOT_PLAYER_PLASMA || s->kind == SHOT_PLAYER_SEISMIC){
      for (int j = 0; j < MAX_ENTITIES; ++j){
        struct Entity* e = &g_entities[j];
        if (!e->active) continue;
        if (e->kind != ENT_GENERATOR && (e->kind < ENT_SCOUT || e->kind > ENT_BOSS)) continue;
        int dx = wrap_delta(s->x - e->x);
        int dy = wrap_delta(s->y - e->y);
        int dz = s->z - e->z;
        int hit_r = (e->kind == ENT_BOSS) ? (5 << FIX_SHIFT) : (3 << FIX_SHIFT);
        if (dx > -hit_r && dx < hit_r && dy > -hit_r && dy < hit_r &&
            dz > -(5 << FIX_SHIFT) && dz < (5 << FIX_SHIFT)){
          if (s->kind == SHOT_PLAYER_SEISMIC){
            detonate_seismic = true;
          } else {
            int dmg = (g_player.weapon_level >= 3) ? 6 : 5;
            if (e->kind == ENT_BOSS && e->shield_gen != 0){
              for (int b = 0; b < 4; ++b){
                if (e->shield_gen & (1 << b)){
                  if (rng_range(3) == 0) e->shield_gen &= ~(1 << b);
                  break;
                }
              }
              dmg = 3;
            }
            e->hp -= dmg;
            e->flash = 5;
            s->active = 0;
            spawn_burst(e->x, e->y, e->z, 4, 0x0FF4, 0x04AF);
            if (e->hp <= 0){
              e->active = 0;
              g_player.kills += 1;
              int pts = (e->kind == ENT_BOSS) ? 2500 : ((e->kind == ENT_GENERATOR) ? 400 : 150);
              g_player.score += pts * g_player.combo;
              g_player.combo = (g_player.combo < 8) ? g_player.combo + 1 : 8;
              g_player.combo_timer = 180;
              spawn_burst(e->x, e->y, e->z, 16, 0x02AF, 0x0FFF);
              sfx_trigger(SFX_ENEMY_EXPLODE);
              if (e->kind == ENT_GENERATOR){
                set_status_banner("SHIELD TOWER DESTROYED! +400", 90);
              }
            } else {
              sfx_trigger(SFX_ENEMY_HIT);
            }
          }
          break;
        }
      }
    } else {
      int dx = wrap_delta(s->x - g_player.x);
      int dy = wrap_delta(s->y - g_player.y);
      int dz = s->z - g_player.z;
      if (dx > -(2 << FIX_SHIFT) && dx < (2 << FIX_SHIFT) &&
          dy > -(2 << FIX_SHIFT) && dy < (2 << FIX_SHIFT) &&
          dz > -(3 << FIX_SHIFT) && dz < (3 << FIX_SHIFT)){
        if (g_player.barrel_spin > 0){
          s->kind = SHOT_PLAYER_PLASMA;
          s->vx = -s->vx;
          s->vy = -s->vy;
          s->color = 0x0FF4;
          s->life = 32;
          g_player.score += 50;
          set_status_banner("BARREL ROLL DEFLECTION! +50", 60);
          sfx_trigger(SFX_BARREL_ROLL);
        } else {
          s->active = 0;
          if (!g_demo_mode){
            g_player.hp -= (s->kind == SHOT_BOSS_ORB) ? 10 : 6;
            g_player.hurt_flash = 10;
            g_player.shake = 5;
            sfx_trigger(SFX_PLAYER_HURT);
          }
        }
      }
    }

    if (detonate_seismic){
      int cx = (s->x >> FIX_SHIFT) & MAP_MASK;
      int cy = (s->y >> FIX_SHIFT) & MAP_MASK;
      terrain_deform(cx, cy, 5, -9, 1);
      spawn_burst(s->x, s->y, s->z + (2 << FIX_SHIFT), 18, 0x02AF, 0x0FF4);
      g_player.shake = 12;
      sfx_trigger(SFX_CRATER_BOOM);
      int splash_r = 12 << FIX_SHIFT;
      for (int j = 0; j < MAX_ENTITIES; ++j){
        struct Entity* e = &g_entities[j];
        if (!e->active) continue;
        if (e->kind != ENT_GENERATOR && (e->kind < ENT_SCOUT || e->kind > ENT_BOSS)) continue;
        int dx = wrap_delta(e->x - s->x);
        int dy = wrap_delta(e->y - s->y);
        if (dx > -splash_r && dx < splash_r && dy > -splash_r && dy < splash_r){
          if (e->kind == ENT_BOSS) e->shield_gen = 0;
          e->hp -= 40;
          e->flash = 8;
          if (e->hp <= 0){
            e->active = 0;
            g_player.kills += 1;
            g_player.score += ((e->kind == ENT_BOSS) ? 2500 : 300) * g_player.combo;
            spawn_burst(e->x, e->y, e->z, 14, 0x02AF, 0x0FFF);
          }
        }
      }
      s->active = 0;
      continue;
    }

    if (s->life <= 0) s->active = 0;
  }

  /* ---- UPDATE ENTITIES & FORWARD CANYON RE-ENGAGEMENT ---- */
  if (g_mode == MODE_RACE) g_player.race_ticks += 1;

  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    if (e->flash > 0) e->flash -= 1;

    if (e->kind == ENT_EXPLOSION){
      e->hp -= 1;
      if (e->hp <= 8) e->phase = 1;
      if (e->hp <= 0) e->active = 0;
      continue;
    }

    int dx = wrap_delta(g_player.x - e->x);
    int dy = wrap_delta(g_player.y - e->y);
    int dist_cells = approx_dist2d(dx >> FIX_SHIFT, dy >> FIX_SHIFT);

    /* Forward Canyon Re-engagement: if a mission target (Shield Tower, Boss,
     * Portal, or Fighter) falls behind the player, smoothly reposition it
     * ahead along the canyon river so action always stays in front! */
    int fwd_from_player = fixmul(-dx, dir_x) + fixmul(-dy, dir_y);
    if (g_mode == MODE_CAMPAIGN && fwd_from_player < -(6 << FIX_SHIFT)){
      if (e->kind == ENT_GENERATOR || e->kind == ENT_PORTAL ||
          (e->kind >= ENT_SCOUT && e->kind <= ENT_BOSS)){
        int ahead_dist = 20 + (i & 15);
        int new_cy = ((g_player.y >> FIX_SHIFT) - ahead_dist) & MAP_MASK;
        int new_cx = (canyon_center_at(new_cy) + ((i & 1) ? 3 : -3)) & MAP_MASK;
        e->x = (new_cx << FIX_SHIFT) + FIX_HALF;
        e->y = (new_cy << FIX_SHIFT) + FIX_HALF;
      }
    }

    if (e->kind == ENT_RING){
      if (dist_cells <= 4){
        /* Recycle ring further ahead down the canyon */
        int new_cy = ((e->y >> FIX_SHIFT) - 48) & MAP_MASK;
        int new_cx = canyon_center_at(new_cy);
        e->x = (new_cx << FIX_SHIFT) + FIX_HALF;
        e->y = (new_cy << FIX_SHIFT) + FIX_HALF;

        g_player.rings_passed += 1;
        g_player.score += 100 * g_player.combo;
        g_player.combo = (g_player.combo < 8) ? g_player.combo + 1 : 8;
        g_player.combo_timer = 180;
        g_player.hp = (g_player.hp + 15 > g_player.max_hp) ? g_player.max_hp : g_player.hp + 15;
        g_player.energy = 100;
        if (g_player.rings_passed == 2 && g_player.weapon_level < 2){
          g_player.weapon_level = 2;
          set_status_banner("QUAD PLASMA CANNONS ONLINE!", 120);
        } else if (g_player.rings_passed == 5 && g_player.weapon_level < 3){
          g_player.weapon_level = 3;
          set_status_banner("HYPER-SPREAD LASERS UNLOCKED!", 120);
        } else {
          set_status_banner("POWER RING! SHIELD & BOOST RECHARGED", 80);
        }
        sfx_trigger(SFX_RING_PASS);
      }
    } else if (e->kind == ENT_PORTAL){
      if (dist_cells <= 5){
        e->active = 0;
        g_player.score += 500 * g_sector;
        sfx_trigger(SFX_SECTOR_CLEAR);
        if (g_demo_mode){
          int next_s = (g_sector % MAX_SECTORS) + 1;
          world_generate(next_s, MODE_CAMPAIGN, g_seed + (unsigned)next_s);
        } else if (g_sector >= MAX_SECTORS){
          scores_record(g_player.score, 0);
          g_state = STATE_VICTORY;
          audio_set_song(SONG_VICTORY);
        } else {
          g_state = STATE_SECTOR_CLEAR;
          audio_set_song(SONG_VICTORY);
        }
        return;
      }
    } else if (e->kind == ENT_GATE){
      if (e->phase == g_player.next_gate && dist_cells <= 5){
        g_player.next_gate += 1;
        g_player.score += 200;
        g_player.energy = 100;
        g_player.speed = 0x6C00;
        sfx_trigger(SFX_RING_PASS);
        if (g_player.next_gate >= RACE_GATE_COUNT){
          g_player.next_gate = 0;
          if (g_player.best_race_ticks == 0 || g_player.race_ticks < g_player.best_race_ticks){
            g_player.best_race_ticks = g_player.race_ticks;
          }
          if (!g_demo_mode){
            scores_record(g_player.score, g_player.race_ticks);
            g_state = STATE_VICTORY;
            audio_set_song(SONG_VICTORY);
          } else {
            g_player.race_ticks = 0;
          }
        }
      }
    } else if (e->kind == ENT_RIVAL){
      e->y = (e->y - 0x3A00) & full_map_mask;
      int rcx = canyon_center_at((e->y >> FIX_SHIFT) & MAP_MASK);
      int target_x = (rcx << FIX_SHIFT) + FIX_HALF;
      int rdx = wrap_delta(target_x - e->x);
      e->x = (e->x + (rdx >> 3)) & full_map_mask;
    } else if (e->kind >= ENT_SCOUT && e->kind <= ENT_BOSS){
      if (e->kind == ENT_SCOUT){
        int sway = isin(((int)g_frame << 4) + (e->phase << 6)) >> 4;
        e->x = (e->x + sway) & full_map_mask;
        e->y = (e->y - 0x2600) & full_map_mask; /* Fly down the canyon ahead of player */
      } else if (e->kind == ENT_INTERCEPTOR || e->kind == ENT_STEALTH){
        int rcx = canyon_center_at((e->y >> FIX_SHIFT) & MAP_MASK);
        int target_x = ((rcx << FIX_SHIFT) + FIX_HALF) + (isin(((int)g_frame << 3) + (i << 7)) << 2);
        int rdx = wrap_delta(target_x - e->x);
        e->x = (e->x + (rdx >> 4)) & full_map_mask;
        e->y = (e->y - 0x2C00) & full_map_mask;
      } else if (e->kind == ENT_BOSS){
        /* Sector Guardian / Leviathan paces 18 cells ahead of player down the canyon! */
        int target_y = (g_player.y - (18 << FIX_SHIFT)) & full_map_mask;
        int b_cy = (target_y >> FIX_SHIFT) & MAP_MASK;
        int b_cx = canyon_center_at(b_cy);
        int target_x = ((b_cx << FIX_SHIFT) + FIX_HALF) + (isin((int)g_frame << 3) * 4);
        e->x = (e->x + (wrap_delta(target_x - e->x) >> 3)) & full_map_mask;
        e->y = (e->y + (wrap_delta(target_y - e->y) >> 2)) & full_map_mask;
        e->z = ((WATER_LEVEL + 15) << FIX_SHIFT) + (icos((int)g_frame << 3) << 1);
      }

      if (e->cooldown > 0){
        e->cooldown -= 1;
      } else if (dist_cells <= 36 && dist_cells >= 5){
        e->cooldown = (e->kind == ENT_BOSS) ? 32 : (52 - g_sector * 4);
        int ticks = dist_cells * 2;
        if (ticks < 1) ticks = 1;
        int vx = dx / ticks;
        int dy_v = dy / ticks;
        int vz = (g_player.z - e->z) / ticks;
        spawn_shot((e->kind == ENT_BOSS) ? SHOT_BOSS_ORB : SHOT_ENEMY_LASER,
                   e->x, e->y, e->z, vx, dy_v, vz, 45,
                   (e->kind == ENT_BOSS) ? 0x0F3F : 0x024F);
      }
    }
  }

  for (int i = 0; i < MAX_PARTICLES; ++i){
    struct Particle* p = &g_particles[i];
    if (!p->active) continue;
    p->x = (p->x + p->vx) & full_map_mask;
    p->y = (p->y + p->vy) & full_map_mask;
    p->z += p->vz;
    p->vz -= 0x0300;
    p->life -= 1;
    if (p->life <= 0) p->active = 0;
  }

  if (g_mode == MODE_CAMPAIGN){
    advance_campaign_phase();
    if (g_player.hp <= 0 && !g_demo_mode && g_state == STATE_PLAY){
      g_player.hp = 0;
      scores_record(g_player.score, 0);
      g_state = STATE_GAMEOVER;
    }
  }
}
