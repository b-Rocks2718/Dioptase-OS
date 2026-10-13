#include "realm.h"
#include "assets.h"
#include "../crt/string.h"

/* Simple linear congruential pseudo-random generator. */
static unsigned rand_state = 123456789;

/* Advance and return a pseudo-random 32-bit integer. */
static unsigned next_rand(void) {
  rand_state = rand_state * 1103515245 + 12345;
  return rand_state;
}

/* Return a pseudo-random integer in the range [0, max - 1]. */
static int rand_range(int max) {
  if (max <= 0) return 0;
  return (int)((next_rand() >> 8) % (unsigned)max);
}

/* Retrieve the cell at tile coordinates (tx, ty) with bounds validation. */
struct MapCell* world_get_cell(struct GameWorld* w, int tx, int ty) {
  if (tx < 0 || tx >= MAP_SIZE || ty < 0 || ty >= MAP_SIZE) {
    return NULL;
  }
  return &w->grid[ty * MAP_SIZE + tx];
}

/* Check whether a tile allows land units to step onto it. */
bool world_is_walkable(struct GameWorld* w, int tx, int ty) {
  struct MapCell* c = world_get_cell(w, tx, ty);
  if (c == NULL) return false;
  if (c->terrain == TERRAIN_WATER) return false;
  if (c->resource != RES_NONE) return false;
  if (c->building_id != -1) return false;
  return true;
}

/* Query footprint dimensions for a building category. */
static void get_building_dims(int type, int* w_out, int* h_out, int* hp_out) {
  switch (type) {
    case BLD_CASTLE:      *w_out = 3; *h_out = 3; *hp_out = 1000; break;
    case BLD_HOUSE:       *w_out = 2; *h_out = 2; *hp_out = 200;  break;
    case BLD_FARM:        *w_out = 2; *h_out = 2; *hp_out = 150;  break;
    case BLD_LUMBER_CAMP: *w_out = 2; *h_out = 2; *hp_out = 250;  break;
    case BLD_BARRACKS:    *w_out = 3; *h_out = 2; *hp_out = 400;  break;
    case BLD_TOWER:       *w_out = 2; *h_out = 2; *hp_out = 350;  break;
    case BLD_WALL:        *w_out = 1; *h_out = 1; *hp_out = 300;  break;
    default:              *w_out = 1; *h_out = 1; *hp_out = 100;  break;
  }
}

/* Check resource affordability and map clearance to construct a building. */
bool world_can_place(struct GameWorld* w, int type, int tx, int ty) {
  int bw, bh, max_hp;
  get_building_dims(type, &bw, &bh, &max_hp);

  if (tx < 2 || tx + bw >= MAP_SIZE - 2 || ty < 2 || ty + bh >= MAP_SIZE - 2) {
    return false;
  }

  /* Check cost requirements */
  switch (type) {
    case BLD_HOUSE:
      if (w->wood < 40) return false;
      break;
    case BLD_FARM:
      if (w->wood < 30) return false;
      break;
    case BLD_LUMBER_CAMP:
      if (w->wood < 50) return false;
      break;
    case BLD_BARRACKS:
      if (w->wood < 70 || w->stone < 40) return false;
      break;
    case BLD_TOWER:
      if (w->wood < 50 || w->stone < 35) return false;
      break;
    case BLD_WALL:
      if (w->stone < 15) return false;
      break;
    default:
      break;
  }

  /* Ensure all occupied cells are free of water, obstacles, and structures */
  for (int y = 0; y < bh; y++) {
    for (int x = 0; x < bw; x++) {
      struct MapCell* c = world_get_cell(w, tx + x, ty + y);
      if (c == NULL || c->terrain == TERRAIN_WATER || c->resource != RES_NONE || c->building_id != -1) {
        return false;
      }
    }
  }

  return true;
}

/* Instantiate and register a building structure on the kingdom grid. */
int world_place_building(struct GameWorld* w, int type, int tx, int ty) {
  if (!world_can_place(w, type, tx, ty)) {
    return -1;
  }

  /* Deduct treasury cost */
  switch (type) {
    case BLD_HOUSE:       w->wood -= 40; break;
    case BLD_FARM:        w->wood -= 30; break;
    case BLD_LUMBER_CAMP: w->wood -= 50; break;
    case BLD_BARRACKS:    w->wood -= 70; w->stone -= 40; break;
    case BLD_TOWER:       w->wood -= 50; w->stone -= 35; break;
    case BLD_WALL:        w->stone -= 15; break;
    default: break;
  }

  /* Locate an unused building slot */
  int slot = -1;
  for (int i = 0; i < MAX_BUILDINGS; i++) {
    if (!w->buildings[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == -1) return -1;

  int bw, bh, max_hp;
  get_building_dims(type, &bw, &bh, &max_hp);

  struct Building* b = &w->buildings[slot];
  b->active = 1;
  b->type = type;
  b->tile_x = tx;
  b->tile_y = ty;
  b->width = bw;
  b->height = bh;
  b->hp = max_hp;
  b->max_hp = max_hp;
  b->build_progress = 100;
  b->cooldown = 0;

  for (int y = 0; y < bh; y++) {
    for (int x = 0; x < bw; x++) {
      struct MapCell* c = world_get_cell(w, tx + x, ty + y);
      if (c != NULL) {
        c->building_id = slot;
      }
    }
  }

  if (type == BLD_HOUSE) {
    w->max_population += 5;
    add_float_text(w, (tx * TILE_PX) + 8, (ty * TILE_PX), "+5 POP", COLOR_RGB(10, 15, 10));
  }

  audio_play_sfx(SFX_BUILD_PLACE);

  /* Placement puff particles */
  for (int p = 0; p < 8; p++) {
    int px = (tx * TILE_PX) + (bw * TILE_PX / 2) + rand_range(16) - 8;
    int py = (ty * TILE_PX) + (bh * TILE_PX / 2) + rand_range(16) - 8;
    add_particle(w, px, py, rand_range(7) - 3, rand_range(7) - 3, PAL_LIGHT_GRAY, 10);
  }

  return slot;
}

/* Generate procedural island terrain, forests, mines, and initial settlement. */
void world_init(struct GameWorld* w, unsigned seed) {
  rand_state = seed;
  memset(w, 0, sizeof(struct GameWorld));

  /* Initialize terrain with coastline and interior fields */
  for (int y = 0; y < MAP_SIZE; y++) {
    for (int x = 0; x < MAP_SIZE; x++) {
      struct MapCell* c = &w->grid[y * MAP_SIZE + x];
      c->building_id = -1;
      c->resource = RES_NONE;
      c->resource_hp = 0;

      /* Determine distance from island center */
      int dx = x - (MAP_SIZE / 2);
      int dy = y - (MAP_SIZE / 2);
      int dist_sq = dx * dx + dy * dy;

      if (dist_sq > (28 * 28)) {
        c->terrain = TERRAIN_WATER;
      } else if (dist_sq > (26 * 26)) {
        c->terrain = TERRAIN_SHORE;
      } else {
        c->terrain = TERRAIN_GRASS;
      }
    }
  }

  /* Scatter clusters of trees, stone deposits, and gold veins */
  for (int i = 0; i < 40; i++) {
    int cx = 10 + rand_range(MAP_SIZE - 20);
    int cy = 10 + rand_range(MAP_SIZE - 20);
    int rtype = RES_TREE;
    int roll = rand_range(100);
    if (roll > 80) rtype = RES_GOLD;
    else if (roll > 60) rtype = RES_STONE;

    int cluster_size = 4 + rand_range(8);
    for (int j = 0; j < cluster_size; j++) {
      int tx = cx + rand_range(5) - 2;
      int ty = cy + rand_range(5) - 2;
      struct MapCell* c = world_get_cell(w, tx, ty);
      if (c != NULL && c->terrain == TERRAIN_GRASS) {
        /* Keep clearing around center (32, 32) */
        int edx = tx - 32;
        int edy = ty - 32;
        if ((edx * edx + edy * edy) > 36) {
          c->resource = rtype;
          c->resource_hp = (rtype == RES_TREE) ? 100 : (rtype == RES_STONE ? 150 : 200);
        }
      }
    }
  }

  /* Set starting stockpile */
  w->wood = 140;
  w->stone = 80;
  w->gold = 50;
  w->food = 90;
  w->population = 0;
  w->max_population = 10;
  w->day_number = 1;
  w->day_ticks = 0;
  w->phase = PHASE_DAWN;
  w->score = 0;
  w->enemies_slain = 0;
  w->auto_commander = 0;
  w->sound_enabled = 1;
  w->game_state = STATE_TITLE;
  w->selected_building_type = BLD_NONE;

  /* Place central Castle stronghold at tile (31, 31) */
  world_place_building(w, BLD_CASTLE, 31, 31);

  /* Spawn initial villagers */
  unit_spawn(w, UNIT_VILLAGER, TEAM_PLAYER, (30 * TILE_PX) << FIXED_SHIFT, (35 * TILE_PX) << FIXED_SHIFT);
  unit_spawn(w, UNIT_VILLAGER, TEAM_PLAYER, (34 * TILE_PX) << FIXED_SHIFT, (35 * TILE_PX) << FIXED_SHIFT);
  unit_spawn(w, UNIT_VILLAGER, TEAM_PLAYER, (32 * TILE_PX) << FIXED_SHIFT, (36 * TILE_PX) << FIXED_SHIFT);

  /* Center camera on castle */
  w->cam_x = (32 * TILE_PX) - (SCREEN_W / 2);
  w->cam_y = (32 * TILE_PX) - (VIEW_H / 2);
  if (w->cam_x < 0) w->cam_x = 0;
  if (w->cam_y < 0) w->cam_y = 0;
}

/* Spawn invading monster host according to current survival day. */
static void spawn_wave(struct GameWorld* w) {
  int count = 2 + (w->day_number * 3);
  int day = w->day_number;

  for (int i = 0; i < count; i++) {
    /* Spawn along island rim */
    int angle_idx = rand_range(16);
    int spawn_tx = 32 + ((angle_idx < 8) ? (angle_idx * 3 - 12) : 22);
    int spawn_ty = (angle_idx < 8) ? 8 : (32 + (angle_idx - 12) * 3);
    if (spawn_tx < 6) spawn_tx = 6;
    if (spawn_tx > MAP_SIZE - 7) spawn_tx = MAP_SIZE - 7;
    if (spawn_ty < 6) spawn_ty = 6;
    if (spawn_ty > MAP_SIZE - 7) spawn_ty = MAP_SIZE - 7;

    int utype = UNIT_GOBLIN;
    int roll = rand_range(100);
    if (day >= 5 && i == 0) {
      utype = UNIT_BOSS;
    } else if (day >= 4 && roll > 75) {
      utype = UNIT_OGRE;
    } else if (day >= 3 && roll > 50) {
      utype = UNIT_SKELETON;
    } else if (day >= 2 && roll > 35) {
      utype = UNIT_ORC;
    }

    unit_spawn(w, utype, TEAM_ENEMY, (spawn_tx * TILE_PX) << FIXED_SHIFT, (spawn_ty * TILE_PX) << FIXED_SHIFT);
  }

  add_float_text(w, (32 * TILE_PX), (28 * TILE_PX), "NIGHTFALL! DEFEND!", PAL_CRIMSON);
}

/* Advance world clock, diurnal cycle, agricultural harvest, and tower defense. */
void world_update(struct GameWorld* w) {
  w->day_ticks++;

  /* Full diurnal cycle = 1200 ticks (~40 seconds) */
  if (w->day_ticks < 200) {
    if (w->phase != PHASE_DAWN) {
      w->phase = PHASE_DAWN;
      audio_play_sfx(SFX_FANFARE_DAWN);
      w->score += 500;
      add_float_text(w, (32 * TILE_PX), (30 * TILE_PX), "+500 PTS", COLOR_RGB(15, 15, 4));
    }
  } else if (w->day_ticks < 750) {
    w->phase = PHASE_DAY;
  } else if (w->day_ticks < 900) {
    if (w->phase != PHASE_DUSK) {
      w->phase = PHASE_DUSK;
      audio_play_sfx(SFX_BELL_DUSK);
      add_float_text(w, (32 * TILE_PX), (30 * TILE_PX), "DUSK BELL TOLLS", COLOR_RGB(15, 10, 2));
    }
  } else if (w->day_ticks < 1200) {
    if (w->phase != PHASE_NIGHT) {
      w->phase = PHASE_NIGHT;
      spawn_wave(w);
    }
  } else {
    /* Day completed */
    w->day_ticks = 0;
    w->day_number++;
  }

  /* Farm crop replenishment */
  if ((w->day_ticks % 90) == 0) {
    for (int i = 0; i < MAX_BUILDINGS; i++) {
      if (w->buildings[i].active && w->buildings[i].type == BLD_FARM) {
        w->food += 4;
      }
    }
  }

  /* Automated tower and castle arrow defenses */
  for (int i = 0; i < MAX_BUILDINGS; i++) {
    struct Building* b = &w->buildings[i];
    if (!b->active) continue;
    if (b->type != BLD_TOWER && b->type != BLD_CASTLE) continue;

    if (b->cooldown > 0) {
      b->cooldown--;
      continue;
    }

    /* Search for nearest hostile creature within firing range */
    int bx = (b->tile_x * TILE_PX) + (b->width * TILE_PX / 2);
    int by = (b->tile_y * TILE_PX) + (b->height * TILE_PX / 2);
    int max_range = (b->type == BLD_TOWER) ? 96 : 80;
    int nearest_enemy = -1;
    int best_dist_sq = max_range * max_range;

    for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
      struct Unit* enemy = &w->units[u];
      if (!enemy->active || enemy->team != TEAM_ENEMY || enemy->state == STATE_DEAD) continue;

      int ex = enemy->x >> FIXED_SHIFT;
      int ey = enemy->y >> FIXED_SHIFT;
      int dsq = (ex - bx) * (ex - bx) + (ey - by) * (ey - by);
      if (dsq < best_dist_sq) {
        best_dist_sq = dsq;
        nearest_enemy = u;
      }
    }

    if (nearest_enemy != -1) {
      struct Unit* target = &w->units[nearest_enemy];
      projectile_spawn(w, TEAM_PLAYER, bx, by, target->x >> FIXED_SHIFT, target->y >> FIXED_SHIFT, 25);
      b->cooldown = (b->type == BLD_TOWER) ? 22 : 30;
      audio_play_sfx(SFX_ARROW_SHOOT);
    }
  }

  /* Check castle health */
  bool castle_standing = false;
  for (int i = 0; i < MAX_BUILDINGS; i++) {
    if (w->buildings[i].active && w->buildings[i].type == BLD_CASTLE) {
      castle_standing = true;
      break;
    }
  }
  if (!castle_standing) {
    w->game_state = STATE_GAMEOVER;
    audio_play_sfx(SFX_DEFEAT);
  }
}
