#include "skyforge.h"

/*
 * Procedural 3D voxel terrain generator, dynamic slope lighting, real-time
 * crater and Terrain Forge sculpting, flight physics, and entity AI.
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
unsigned g_seed = 0x5A17F04E;
bool g_autopilot = false;
bool g_demo_mode = false;
char g_banner_msg[40];
int g_banner_timer = 0;

static unsigned rng_state = 0x5A17F04E;

/* Seed the deterministic xorshift32 generator. */
void rng_seed(unsigned s){
  rng_state = s ? s : 0x5A17F04E;
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

/* Copy a short message into the HUD banner for `duration_ticks` frames. */
void set_status_banner(char* msg, int duration_ticks){
  int i = 0;
  while (msg[i] && i < 39){
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
static int canyon_center_x(int cy, unsigned seed_off){
  int a1 = ((cy << 2) + (int)(seed_off & 0x3FF)) & ANGLE_MASK;
  int a2 = ((cy << 3) + (int)((seed_off >> 5) & 0x3FF)) & ANGLE_MASK;
  int sway = (isin(a1) * 11 + icos(a2) * 5) >> FIX_SHIFT;
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
      /* Preserve explicit crystal/lava accent paint (light == 3) above water */
      int light = 1;
      if (old_light == 3 && h > WATER_LEVEL){
        light = 3;
      } else if (h <= WATER_LEVEL){
        light = ((wx + wy) & 3) == 0 ? 2 : 1;
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
      int falloff = radius - (approx_dist2d(dx, dy));
      if (falloff < 1) falloff = 1;
      int dh = (delta_h * falloff) / radius;
      h += dh;
      if (h < WATER_LEVEL - 2) h = WATER_LEVEL - 2;
      if (h > MAX_HEIGHT) h = MAX_HEIGHT;
      int light = (g_terrain[idx] >> 14) & 3;
      if (paint_kind == 1){
        /* Crystal / ember rim accent */
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
    /* Raise a rim first, then hollow out the caldera center */
    terrain_deform(cx, cy, radius + 2, 5, 1);
    terrain_deform(cx, cy, radius - 1, -10, 3);
  } else if (tool == TOOL_CRYSTAL){
    terrain_deform(cx, cy, radius, inverse ? 0 : 2, 2);
  } else if (tool == TOOL_LAVA){
    terrain_deform(cx, cy, radius, -8, 3);
  } else if (tool == TOOL_RING){
    /* Spawn or move a power ring / arch at the sculpted cell */
    for (int i = 0; i < MAX_ENTITIES; ++i){
      if (!g_entities[i].active){
        struct Entity* e = &g_entities[i];
        e->active = 1;
        e->kind = inverse ? ENT_ARCH : ENT_RING;
        e->x = ((cx & MAP_MASK) << FIX_SHIFT) + FIX_HALF;
        e->y = ((cy & MAP_MASK) << FIX_SHIFT) + FIX_HALF;
        int th = terrain_height_cell(cx, cy);
        if (th < WATER_LEVEL) th = WATER_LEVEL;
        e->z = (th + 10) << FIX_SHIFT;
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

/* Spawn an explosion burst of `count` 3D particles at (x, y, z). */
static void spawn_burst(int x, int y, int z, int count, unsigned short c1, unsigned short c2){
  for (int i = 0; i < count; ++i){
    int ang = rng_range(ANGLE_COUNT);
    int spd = (rng_range(12) + 4) << 11;
    int vx = fixmul(icos(ang), spd);
    int vy = fixmul(isin(ang), spd);
    int vz = (rng_range(16) - 6) << 12;
    spawn_particle(x, y, z, vx, vy, vz, 14 + rng_range(14), (i & 1) ? c1 : c2);
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

/* Count remaining hostile entities in Campaign mode. */
int world_enemies_remaining(void){
  int count = 0;
  for (int i = 0; i < MAX_ENTITIES; ++i){
    if (!g_entities[i].active) continue;
    int k = g_entities[i].kind;
    if (k == ENT_SCOUT || k == ENT_INTERCEPTOR || k == ENT_TURRET ||
        k == ENT_STEALTH || k == ENT_BOSS){
      count += 1;
    }
  }
  return count;
}

/* Generate a 128x128 3D terrain, canyon river, arches, rings, and enemies. */
void world_generate(int sector, int mode, unsigned seed){
  if (sector < 1) sector = 1;
  if (sector > MAX_SECTORS) sector = MAX_SECTORS;
  g_sector = sector;
  g_mode = mode;
  g_biome = (sector - 1) % BIOME_COUNT;
  rng_seed(seed ^ (unsigned)(sector * 0x9E3779B9));

  /* Clear entities, shots, and particles */
  for (int i = 0; i < MAX_ENTITIES; ++i) g_entities[i].active = 0;
  for (int i = 0; i < MAX_SHOTS; ++i) g_shots[i].active = 0;
  for (int i = 0; i < MAX_PARTICLES; ++i) g_particles[i].active = 0;

  unsigned seed_off = rng_next();

  /* Synthesize multi-harmonic terrain heightmap with a winding canyon river */
  for (int cy = 0; cy < MAP_SIZE; ++cy){
    int riv_x = canyon_center_x(cy, seed_off);
    for (int cx = 0; cx < MAP_SIZE; ++cx){
      int dx = cx - riv_x;
      if (dx > 64) dx -= 128;
      if (dx < -64) dx += 128;
      int adx = dx < 0 ? -dx : dx;

      int ax = ((cx << 4) + (int)(seed_off & 0x1FF)) & ANGLE_MASK;
      int ay = ((cy << 4) + (int)((seed_off >> 4) & 0x1FF)) & ANGLE_MASK;
      int axy = (((cx + cy) << 3) + (int)((seed_off >> 8) & 0x1FF)) & ANGLE_MASK;
      int ridge = ((isin(ax) + icos(ay)) * 11 + isin(axy) * 8) >> FIX_SHIFT;

      int h = WATER_LEVEL;
      if (adx <= 8){
        /* Wide canyon river channel */
        h = WATER_LEVEL - 2 + (adx >> 2);
      } else if (adx <= 13){
        /* Gentle shoreline and lowland banks */
        h = WATER_LEVEL + (adx - 8);
      } else if (adx <= 24){
        /* Terraced canyon cliffs rising on both sides */
        h = WATER_LEVEL + 5 + ((adx - 13) * 2) + (ridge >> 1);
      } else {
        /* Towering mountain peaks and mesas */
        h = 30 + ridge + ((adx - 24) >> 1);
      }
      if (h < 2) h = 2;
      if (h > MAX_HEIGHT) h = MAX_HEIGHT;

      /* Place glowing crystal / magma seams on high canyon ledges */
      int light = 1;
      if (h >= 20 && h <= 38 && ((cx ^ cy) & 15) == 0){
        light = 3;
      }
      g_terrain[(cy << MAP_SHIFT) | cx] = (unsigned short)(h | (((light << 6) | h) << 8));
    }
  }
  terrain_relight_rect(0, 0, MAP_SIZE - 1, MAP_SIZE - 1);

  /* Place player at the south mouth of the canyon facing north (-Y, angle 768) */
  int start_cy = 112;
  int start_cx = canyon_center_x(start_cy, seed_off);
  bool fresh_run = (sector == 1) || (g_player.hp <= 0);
  int prev_score = fresh_run ? 0 : g_player.score;
  int prev_kills = fresh_run ? 0 : g_player.kills;
  int prev_wpn = fresh_run ? 1 : g_player.weapon_level;
  int prev_best_race = g_player.best_race_ticks;

  g_player.x = (start_cx << FIX_SHIFT) + FIX_HALF;
  g_player.y = (start_cy << FIX_SHIFT) + FIX_HALF;
  g_player.z = (WATER_LEVEL + 16) << FIX_SHIFT;
  g_player.angle = 768; /* Facing north along canyon */
  g_player.pitch = 0;
  g_player.roll = 0;
  g_player.barrel_spin = 0;
  g_player.barrel_cooldown = 0;
  g_player.speed = 0x3800; /* ~0.22 cells/tick */
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
  g_player.beacons_saved = 0;
  g_player.next_gate = 0;
  g_player.lap = 1;
  g_player.race_ticks = 0;
  g_player.best_race_ticks = prev_best_race;
  g_player.aim_x = 0;
  g_player.aim_y = 0;
  g_player.lock_target = -1;
  g_player.forge_tool = TOOL_RAISE;
  g_player.forge_radius = 4;
  g_player.forge_cursor_x = start_cx;
  g_player.forge_cursor_y = (start_cy - 10) & MAP_MASK;

  int ent_idx = 0;

  if (mode == MODE_RACE){
    /* Place 24 sequential Grand Prix gates along the canyon river */
    for (int g = 0; g < RACE_GATE_COUNT && ent_idx < MAX_ENTITIES; ++g){
      int cy = (start_cy - 5 - g * 5) & MAP_MASK;
      int cx = canyon_center_x(cy, seed_off);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_GATE;
      e->x = (cx << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      int th = terrain_height_cell(cx, cy);
      if (th < WATER_LEVEL) th = WATER_LEVEL;
      e->z = (th + 10 + ((g & 1) << 1)) << FIX_SHIFT;
      e->phase = g;
      e->hp = 100;
      e->max_hp = 100;
    }
    /* Place 6 canyon stone arches and 3 Rival AI racers */
    for (int a = 0; a < 6 && ent_idx < MAX_ENTITIES; ++a){
      int cy = (start_cy - 12 - a * 20) & MAP_MASK;
      int cx = canyon_center_x(cy, seed_off);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_ARCH;
      e->x = (cx << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      e->z = (WATER_LEVEL + 16) << FIX_SHIFT;
      e->hp = 200;
      e->max_hp = 200;
    }
    for (int r = 0; r < 3 && ent_idx < MAX_ENTITIES; ++r){
      int cy = (start_cy - 6 - r * 12) & MAP_MASK;
      int cx = canyon_center_x(cy, seed_off);
      struct Entity* e = &g_entities[ent_idx++];
      e->active = 1;
      e->kind = ENT_RIVAL;
      e->x = ((cx + (r - 1) * 2) << FIX_SHIFT) + FIX_HALF;
      e->y = (cy << FIX_SHIFT) + FIX_HALF;
      e->z = (WATER_LEVEL + 12) << FIX_SHIFT;
      e->angle = 768;
      e->phase = r;
      e->hp = 50;
      e->max_hp = 50;
    }
    set_status_banner("CANYON GRAND PRIX: FLY THROUGH GATES!", 150);
    return;
  }

  /* Campaign & Terrain Forge: place power rings, canyon arches, beacons, and enemies */
  for (int r = 0; r < 10 && ent_idx < MAX_ENTITIES; ++r){
    int cy = (start_cy - 10 - r * 12) & MAP_MASK;
    int cx = canyon_center_x(cy, seed_off);
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    e->kind = (r & 3) == 3 ? ENT_ARCH : ENT_RING;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    int th = terrain_height_cell(cx, cy);
    if (th < WATER_LEVEL) th = WATER_LEVEL;
    e->z = (th + 11) << FIX_SHIFT;
    e->hp = 50;
    e->max_hp = 50;
    e->phase = r;
  }

  /* Place crystal spires and rescue beacons on canyon banks */
  for (int b = 0; b < 8 && ent_idx < MAX_ENTITIES; ++b){
    int cy = (start_cy - 14 - b * 15) & MAP_MASK;
    int side = (b & 1) ? 8 : -8;
    int cx = (canyon_center_x(cy, seed_off) + side) & MAP_MASK;
    int th = terrain_height_cell(cx, cy);
    if (th < WATER_LEVEL) th = WATER_LEVEL;
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    e->kind = (b & 1) ? ENT_BEACON : ENT_SPIRE;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    e->z = (th + 4) << FIX_SHIFT;
    e->hp = 30;
    e->max_hp = 30;
    e->phase = b;
  }

  if (mode == MODE_FORGE){
    set_status_banner("TERRAIN FORGE: SCULPT 3D WORLD LIVE!", 150);
    return;
  }

  /* Spawn campaign敌 hostile forces scaled by sector (1..5) */
  int num_enemies = 8 + sector * 2;
  for (int m = 0; m < num_enemies && ent_idx < MAX_ENTITIES - 1; ++m){
    int cy = (start_cy - 16 - m * 7) & MAP_MASK;
    int cx = (canyon_center_x(cy, seed_off) + rng_range(15) - 7) & MAP_MASK;
    int th = terrain_height_cell(cx, cy);
    if (th < WATER_LEVEL) th = WATER_LEVEL;
    struct Entity* e = &g_entities[ent_idx++];
    e->active = 1;
    if ((m & 3) == 0){
      e->kind = ENT_TURRET;
      e->z = (th + 3) << FIX_SHIFT;
      e->hp = 18 + sector * 4;
    } else if ((m & 3) == 1 && sector >= 2){
      e->kind = ENT_INTERCEPTOR;
      e->z = (th + 13) << FIX_SHIFT;
      e->hp = 14 + sector * 3;
    } else if ((m & 3) == 2 && sector >= 3){
      e->kind = ENT_STEALTH;
      e->z = (th + 16) << FIX_SHIFT;
      e->hp = 16 + sector * 3;
    } else {
      e->kind = ENT_SCOUT;
      e->z = (th + 11) << FIX_SHIFT;
      e->hp = 10 + sector * 2;
    }
    e->max_hp = e->hp;
    e->x = (cx << FIX_SHIFT) + FIX_HALF;
    e->y = (cy << FIX_SHIFT) + FIX_HALF;
    e->angle = 256; /* Facing south toward player */
    e->cooldown = 25 + (m << 3);
    e->phase = m;
  }

  /* Sector 5 spawns the Leviathan Dreadnought Boss! */
  if (sector == MAX_SECTORS && ent_idx < MAX_ENTITIES){
    int cy = (start_cy - 45) & MAP_MASK;
    int cx = canyon_center_x(cy, seed_off);
    struct Entity* boss = &g_entities[ent_idx++];
    boss->active = 1;
    boss->kind = ENT_BOSS;
    boss->x = (cx << FIX_SHIFT) + FIX_HALF;
    boss->y = (cy << FIX_SHIFT) + FIX_HALF;
    boss->z = (WATER_LEVEL + 20) << FIX_SHIFT;
    boss->hp = 180;
    boss->max_hp = 180;
    boss->cooldown = 40;
    boss->shield_gen = 0x0F;
    sfx_trigger(SFX_BOSS_ALARM);
    set_status_banner("WARNING: LEVIATHAN DREADNOUGHT!", 180);
  } else {
    set_status_banner("SECTOR LAUNCH: CLEAR THE CANYON!", 140);
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

/* Advance player flight, weapons, Terrain Forge, entities, and collisions. */
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

  /* 1. Barrel-roll spin and energy regeneration */
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

  /* 2. Yaw, Roll banking, and Pitch */
  int turn_step = ctrl->turn * 10;
  g_player.angle = (g_player.angle + turn_step) & ANGLE_MASK;
  int target_roll = ctrl->turn * 18;
  if (target_roll > 28) target_roll = 28;
  if (target_roll < -28) target_roll = -28;
  if (g_player.roll < target_roll) g_player.roll += 2;
  else if (g_player.roll > target_roll) g_player.roll -= 2;

  int target_pitch = ctrl->pitch * 24;
  if (g_player.pitch < target_pitch) g_player.pitch += 2;
  else if (g_player.pitch > target_pitch) g_player.pitch -= 2;
  if (g_player.pitch > 40) g_player.pitch = 40;
  if (g_player.pitch < -40) g_player.pitch = -40;

  /* 3. Speed / Afterburner throttle */
  int target_speed = (g_mode == MODE_FORGE) ? 0x2000 : 0x3C00;
  g_player.boost_active = 0;
  if (ctrl->throttle > 0 && g_player.energy > 2){
    target_speed = (g_mode == MODE_FORGE) ? 0x5000 : 0x6800;
    g_player.boost_active = 1;
    if ((g_frame & 1) == 0 && !g_demo_mode) g_player.energy -= 1;
  } else if (ctrl->throttle < 0){
    target_speed = (g_mode == MODE_FORGE) ? 0 : 0x1E00;
    if (g_player.energy < 100 && (g_frame & 1) == 0) g_player.energy += 1;
  } else {
    if (g_player.energy < 100 && (g_frame & 1) == 0) g_player.energy += 1;
  }
  if (g_player.speed < target_speed) g_player.speed += 0x0400;
  else if (g_player.speed > target_speed) g_player.speed -= 0x0400;

  /* 4. Advance player 3D position */
  int dir_x = icos(g_player.angle);
  int dir_y = isin(g_player.angle);
  int full_map_mask = (MAP_SIZE << FIX_SHIFT) - 1;
  g_player.x = (g_player.x + fixmul(dir_x, g_player.speed)) & full_map_mask;
  g_player.y = (g_player.y + fixmul(dir_y, g_player.speed)) & full_map_mask;
  g_player.z += (g_player.pitch << 11) + ((g_player.aim_y < -8 ? 1 : (g_player.aim_y > 8 ? -1 : 0)) << 13);

  /* Clamp altitude to terrain floor and stratosphere ceiling */
  int ground_z = terrain_height_at(g_player.x, g_player.y);
  int water_z = WATER_LEVEL << FIX_SHIFT;
  int min_z = (ground_z > water_z ? ground_z : water_z) + (6 << FIX_SHIFT);
  int max_z = (MAX_HEIGHT + 18) << FIX_SHIFT;
  if (g_player.z < min_z){
    g_player.z = min_z;
    if (g_mode != MODE_FORGE && ground_z > water_z + (4 << FIX_SHIFT) && g_player.hurt_flash == 0){
      if (!g_demo_mode){
        g_player.hp -= 5;
        g_player.hurt_flash = 12;
        g_player.shake = 8;
        sfx_trigger(SFX_PLAYER_HURT);
      }
      g_player.pitch = 12;
    }
  }
  if (g_player.z > max_z) g_player.z = max_z;

  /* Update 3D cursor cell ahead of craft for Terrain Forge */
  int look_dist = (12 + (g_player.aim_y >> 3)) << FIX_SHIFT;
  if (look_dist < (5 << FIX_SHIFT)) look_dist = 5 << FIX_SHIFT;
  int cur_wx = g_player.x + fixmul(dir_x, look_dist) +
               fixmul(-dir_y, g_player.aim_x << 10);
  int cur_wy = g_player.y + fixmul(dir_y, look_dist) +
               fixmul(dir_x, g_player.aim_x << 10);
  g_player.forge_cursor_x = (cur_wx >> FIX_SHIFT) & MAP_MASK;
  g_player.forge_cursor_y = (cur_wy >> FIX_SHIFT) & MAP_MASK;

  if (g_mode == MODE_FORGE){
    if ((ctrl->sculpt_apply || ctrl->sculpt_erase) && (g_frame & 3) == 0){
      forge_apply_tool(g_player.forge_cursor_x, g_player.forge_cursor_y,
                       g_player.forge_tool, g_player.forge_radius,
                       ctrl->sculpt_erase != 0);
      sfx_trigger(SFX_SCULPT_HUM);
    }
  }

  /* 5. Acquire nearest lock-on target in front of the player */
  g_player.lock_target = -1;
  int best_dist = 42 << FIX_SHIFT;
  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    if (e->kind < ENT_SCOUT || e->kind > ENT_BOSS) continue;
    int dx = wrap_delta(e->x - g_player.x);
    int dy = wrap_delta(e->y - g_player.y);
    int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    if (fwd <= (2 << FIX_SHIFT) || fwd >= best_dist) continue;
    int side = fixmul(dx, -dir_y) + fixmul(dy, dir_x);
    if (side < 0) side = -side;
    if (side < (fwd >> 1)){
      best_dist = fwd;
      g_player.lock_target = i;
    }
  }

  /* 6. Fire Twin/Quad/Hyper-Spread Plasma Cannons */
  if (ctrl->fire_primary && g_player.fire_cooldown == 0){
    g_player.fire_cooldown = (g_player.weapon_level >= 3) ? 5 : 7;
    int shot_spd = g_player.speed + 0xB000;
    int aim_ang = (g_player.angle + (g_player.aim_x >> 2)) & ANGLE_MASK;
    int sx = icos(aim_ang);
    int sy = isin(aim_ang);
    int vx = fixmul(sx, shot_spd);
    int vy = fixmul(sy, shot_spd);
    int vz = (g_player.pitch - (g_player.aim_y >> 1)) << 11;

    /* Homing assist when locked onto a target */
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

    int right_x = -dir_y;
    int right_y = dir_x;
    spawn_shot(SHOT_PLAYER_PLASMA,
               g_player.x + (right_x >> 1), g_player.y + (right_y >> 1),
               g_player.z - (1 << FIX_SHIFT),
               vx, vy, vz, 36, 0x0FF4);
    spawn_shot(SHOT_PLAYER_PLASMA,
               g_player.x - (right_x >> 1), g_player.y - (right_y >> 1),
               g_player.z - (1 << FIX_SHIFT),
               vx, vy, vz, 36, 0x0FF4);
    if (g_player.weapon_level >= 2){
      spawn_shot(SHOT_PLAYER_PLASMA,
                 g_player.x, g_player.y, g_player.z,
                 vx + (right_x >> 2), vy + (right_y >> 2), vz, 36, 0x04FF);
      spawn_shot(SHOT_PLAYER_PLASMA,
                 g_player.x, g_player.y, g_player.z,
                 vx - (right_x >> 2), vy - (right_y >> 2), vz, 36, 0x04FF);
    }
    sfx_trigger(SFX_PLASMA);
  }

  /* 7. Launch Seismic Nova Torpedo (carves real 3D crater on impact!) */
  if (ctrl->fire_seismic && g_player.bomb_cooldown == 0 && g_player.bombs > 0){
    g_player.bomb_cooldown = 35;
    if (!g_demo_mode && g_mode != MODE_FORGE) g_player.bombs -= 1;
    int shot_spd = g_player.speed + 0x7800;
    int vx = fixmul(dir_x, shot_spd);
    int vy = fixmul(dir_y, shot_spd);
    int vz = -0x3800 + (g_player.pitch << 10);
    spawn_shot(SHOT_PLAYER_SEISMIC,
               g_player.x + dir_x, g_player.y + dir_y,
               g_player.z - (1 << FIX_SHIFT),
               vx, vy, vz, 55, 0x02AF);
    sfx_trigger(SFX_SEISMIC_LAUNCH);
  }

  /* 8. Update in-flight shots and terrain/entity impacts */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    s->x = (s->x + s->vx) & full_map_mask;
    s->y = (s->y + s->vy) & full_map_mask;
    s->z += s->vz;
    if (s->kind == SHOT_PLAYER_SEISMIC){
      s->vz -= 0x0280; /* Gravity arc on seismic torpedo */
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
        spawn_particle(s->x, s->y, gz + FIX_ONE, 0, 0, 0x2000, 6, s->color);
        s->active = 0;
        continue;
      }
    }

    if (s->kind == SHOT_PLAYER_PLASMA || s->kind == SHOT_PLAYER_SEISMIC){
      /* Check collision against hostile entities */
      for (int j = 0; j < MAX_ENTITIES; ++j){
        struct Entity* e = &g_entities[j];
        if (!e->active || e->kind < ENT_SCOUT || e->kind > ENT_BOSS) continue;
        int dx = wrap_delta(s->x - e->x);
        int dy = wrap_delta(s->y - e->y);
        int dz = s->z - e->z;
        int hit_r = (e->kind == ENT_BOSS) ? (5 << FIX_SHIFT) : (2 << FIX_SHIFT) + FIX_HALF;
        if (dx > -hit_r && dx < hit_r && dy > -hit_r && dy < hit_r &&
            dz > -(4 << FIX_SHIFT) && dz < (4 << FIX_SHIFT)){
          if (s->kind == SHOT_PLAYER_SEISMIC){
            detonate_seismic = true;
          } else {
            int dmg = (g_player.weapon_level >= 3) ? 5 : 4;
            if (e->kind == ENT_BOSS && e->shield_gen != 0){
              /* Damage one shield pylon first */
              for (int b = 0; b < 4; ++b){
                if (e->shield_gen & (1 << b)){
                  if (rng_range(4) == 0) e->shield_gen &= ~(1 << b);
                  break;
                }
              }
              dmg = 2;
            }
            e->hp -= dmg;
            e->flash = 5;
            s->active = 0;
            spawn_burst(e->x, e->y, e->z, 4, 0x0FF4, 0x04AF);
            if (e->hp <= 0){
              e->active = 0;
              g_player.kills += 1;
              int pts = (e->kind == ENT_BOSS) ? 2500 : 150;
              g_player.score += pts * g_player.combo;
              g_player.combo = (g_player.combo < 8) ? g_player.combo + 1 : 8;
              g_player.combo_timer = 180;
              spawn_burst(e->x, e->y, e->z, 16, 0x02AF, 0x0FFF);
              sfx_trigger(SFX_ENEMY_EXPLODE);
            } else {
              sfx_trigger(SFX_ENEMY_HIT);
            }
          }
          break;
        }
      }
    } else {
      /* Enemy laser or boss orb vs player */
      int dx = wrap_delta(s->x - g_player.x);
      int dy = wrap_delta(s->y - g_player.y);
      int dz = s->z - g_player.z;
      if (dx > -(2 << FIX_SHIFT) && dx < (2 << FIX_SHIFT) &&
          dy > -(2 << FIX_SHIFT) && dy < (2 << FIX_SHIFT) &&
          dz > -(3 << FIX_SHIFT) && dz < (3 << FIX_SHIFT)){
        if (g_player.barrel_spin > 0){
          /* Barrel roll deflects enemy lasers back! */
          s->kind = SHOT_PLAYER_PLASMA;
          s->vx = -s->vx;
          s->vy = -s->vy;
          s->color = 0x0FF4;
          s->life = 30;
          g_player.score += 25;
          sfx_trigger(SFX_BARREL_ROLL);
        } else {
          s->active = 0;
          if (!g_demo_mode && g_mode != MODE_FORGE){
            g_player.hp -= (s->kind == SHOT_BOSS_ORB) ? 12 : 7;
            g_player.hurt_flash = 10;
            g_player.shake = 6;
            sfx_trigger(SFX_PLAYER_HURT);
          }
        }
      }
    }

    if (detonate_seismic){
      int cx = (s->x >> FIX_SHIFT) & MAP_MASK;
      int cy = (s->y >> FIX_SHIFT) & MAP_MASK;
      terrain_deform(cx, cy, 5, -9, 1);
      spawn_burst(s->x, s->y, s->z + (2 << FIX_SHIFT), 20, 0x02AF, 0x0FF4);
      g_player.shake = 14;
      sfx_trigger(SFX_CRATER_BOOM);
      /* Splash damage to all enemies within 10 cells */
      int splash_r = 10 << FIX_SHIFT;
      for (int j = 0; j < MAX_ENTITIES; ++j){
        struct Entity* e = &g_entities[j];
        if (!e->active || e->kind < ENT_SCOUT || e->kind > ENT_BOSS) continue;
        int dx = wrap_delta(e->x - s->x);
        int dy = wrap_delta(e->y - s->y);
        if (dx > -splash_r && dx < splash_r && dy > -splash_r && dy < splash_r){
          if (e->kind == ENT_BOSS) e->shield_gen = 0;
          e->hp -= 35;
          e->flash = 8;
          if (e->hp <= 0){
            e->active = 0;
            g_player.kills += 1;
            g_player.score += ((e->kind == ENT_BOSS) ? 2500 : 150) * g_player.combo;
            spawn_burst(e->x, e->y, e->z, 14, 0x02AF, 0x0FFF);
          }
        }
      }
      s->active = 0;
      continue;
    }

    if (s->life <= 0) s->active = 0;
  }

  /* 9. Update entities (rings, gates, beacons, enemy AI, boss) */
  if (g_mode == MODE_RACE) g_player.race_ticks += 1;

  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    if (e->flash > 0) e->flash -= 1;

    int dx = wrap_delta(g_player.x - e->x);
    int dy = wrap_delta(g_player.y - e->y);
    int dist_cells = approx_dist2d(dx >> FIX_SHIFT, dy >> FIX_SHIFT);

    if (e->kind == ENT_RING){
      if (dist_cells <= 3){
        e->active = 0;
        g_player.rings_passed += 1;
        g_player.score += 100 * g_player.combo;
        g_player.combo = (g_player.combo < 8) ? g_player.combo + 1 : 8;
        g_player.combo_timer = 180;
        g_player.hp = (g_player.hp + 15 > g_player.max_hp) ? g_player.max_hp : g_player.hp + 15;
        g_player.energy = 100;
        if (g_player.rings_passed == 3 && g_player.weapon_level < 2){
          g_player.weapon_level = 2;
          set_status_banner("QUAD PLASMA CANNONS ONLINE!", 120);
        } else if (g_player.rings_passed == 6 && g_player.weapon_level < 3){
          g_player.weapon_level = 3;
          set_status_banner("HYPER-SPREAD LASERS UNLOCKED!", 120);
        } else {
          set_status_banner("POWER RING! SHIELD & ENERGY RESTORED", 80);
        }
        sfx_trigger(SFX_RING_PASS);
      }
    } else if (e->kind == ENT_GATE){
      if (e->phase == g_player.next_gate && dist_cells <= 4){
        g_player.next_gate += 1;
        g_player.score += 200;
        g_player.energy = 100;
        g_player.speed = 0x7000;
        sfx_trigger(SFX_RING_PASS);
        if (g_player.next_gate >= RACE_GATE_COUNT){
          g_player.next_gate = 0;
          if (g_player.best_race_ticks == 0 || g_player.race_ticks < g_player.best_race_ticks){
            g_player.best_race_ticks = g_player.race_ticks;
          }
          if (!g_demo_mode){
            scores_record(g_player.score, g_player.race_ticks);
            g_state = STATE_VICTORY;
            sfx_trigger(SFX_SECTOR_CLEAR);
          } else {
            g_player.race_ticks = 0;
          }
        }
      }
    } else if (e->kind == ENT_BEACON){
      if (dist_cells <= 3){
        e->active = 0;
        g_player.beacons_saved += 1;
        g_player.bombs += 1;
        g_player.score += 250;
        set_status_banner("DIOPTASE BEACON SECURED! +1 SEISMIC BOMB", 100);
        sfx_trigger(SFX_BEACON_PICKUP);
      }
    } else if (e->kind == ENT_RIVAL){
      /* Rival AI racer weaves northward along the canyon */
      e->y = (e->y - 0x3A00) & full_map_mask;
      int rcx = canyon_center_x((e->y >> FIX_SHIFT) & MAP_MASK, g_seed);
      int target_x = (rcx << FIX_SHIFT) + FIX_HALF;
      int rdx = wrap_delta(target_x - e->x);
      e->x = (e->x + (rdx >> 3)) & full_map_mask;
    } else if (e->kind >= ENT_SCOUT && e->kind <= ENT_BOSS){
      /* Hostile movement and weapon firing */
      if (e->kind == ENT_SCOUT){
        int sway = isin(((int)g_frame << 4) + (e->phase << 6)) >> 4;
        e->x = (e->x + sway) & full_map_mask;
        e->y = (e->y + 0x1200) & full_map_mask;
      } else if (e->kind == ENT_INTERCEPTOR || e->kind == ENT_STEALTH){
        int step_x = dx > 0 ? 0x1600 : -0x1600;
        int step_y = dy > 0 ? 0x1600 : -0x1600;
        e->x = (e->x + step_x) & full_map_mask;
        e->y = (e->y + step_y) & full_map_mask;
        int gz = terrain_height_at(e->x, e->y) + (10 << FIX_SHIFT);
        if (e->z < gz) e->z = gz;
      } else if (e->kind == ENT_BOSS){
        int hover = isin((int)g_frame << 3) >> 3;
        e->x = (e->x + hover) & full_map_mask;
        e->z = ((WATER_LEVEL + 20) << FIX_SHIFT) + (icos((int)g_frame << 3) << 1);
      } else if (e->kind == ENT_TURRET){
        e->z = terrain_height_at(e->x, e->y) + (3 << FIX_SHIFT);
      }

      if (e->cooldown > 0){
        e->cooldown -= 1;
      } else if (dist_cells <= 38 && dist_cells >= 4){
        e->cooldown = (e->kind == ENT_BOSS) ? 28 : (48 - g_sector * 4);
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

  /* 10. Advance 3D particles */
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

  /* 11. Check campaign sector completion or player defeat */
  if (g_mode == MODE_CAMPAIGN && g_state == STATE_PLAY){
    if (g_player.hp <= 0 && !g_demo_mode){
      g_player.hp = 0;
      scores_record(g_player.score, 0);
      g_state = STATE_GAMEOVER;
    } else if (world_enemies_remaining() == 0){
      g_player.score += 500 * g_sector;
      sfx_trigger(SFX_SECTOR_CLEAR);
      if (g_demo_mode){
        int next_s = (g_sector % MAX_SECTORS) + 1;
        world_generate(next_s, MODE_CAMPAIGN, g_seed + next_s);
      } else if (g_sector >= MAX_SECTORS){
        scores_record(g_player.score, 0);
        g_state = STATE_VICTORY;
      } else {
        g_state = STATE_SECTOR_CLEAR;
      }
    }
  }
}
