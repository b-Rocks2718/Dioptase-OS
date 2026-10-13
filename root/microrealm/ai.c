#include "realm.h"
#include "assets.h"

static int ai_tick_timer = 0;

/* Find an unoccupied, walkable tile region suitable for building placement. */
static bool find_build_spot(struct GameWorld* w, int bld_type, int* tx_out, int* ty_out) {
  int center_x = 31;
  int center_y = 31;

  for (int radius = 3; radius < 14; radius++) {
    for (int dy = -radius; dy <= radius; dy += 2) {
      for (int dx = -radius; dx <= radius; dx += 2) {
        int tx = center_x + dx;
        int ty = center_y + dy;
        if (world_can_place(w, bld_type, tx, ty)) {
          *tx_out = tx;
          *ty_out = ty;
          return true;
        }
      }
    }
  }
  return false;
}

/* Locate the closest harvestable resource cell of a specific category. */
static bool find_nearest_resource(struct GameWorld* w, int start_tx, int start_ty, int res_type, int* out_tx, int* out_ty) {
  int best_dsq = 999999;
  bool found = false;

  for (int ty = 4; ty < MAP_SIZE - 4; ty++) {
    for (int tx = 4; tx < MAP_SIZE - 4; tx++) {
      struct MapCell* c = world_get_cell(w, tx, ty);
      if (c != NULL && c->resource == res_type) {
        int dx = tx - start_tx;
        int dy = ty - start_ty;
        int dsq = dx * dx + dy * dy;
        if (dsq < best_dsq) {
          best_dsq = dsq;
          *out_tx = tx;
          *out_ty = ty;
          found = true;
        }
      }
    }
  }
  return found;
}

/* Autonomous AI commander logic executing strategic governance and defense. */
void ai_commander_step(struct GameWorld* w) {
  ai_tick_timer++;
  if ((ai_tick_timer % 20) != 0) return;

  /* Count population categories */
  int villager_count = 0;
  int military_count = 0;
  int tower_count = 0;
  int barracks_count = 0;

  for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
    struct Unit* unit = &w->units[u];
    if (!unit->active || unit->team != TEAM_PLAYER || unit->hp <= 0) continue;
    if (unit->type == UNIT_VILLAGER) villager_count++;
    else military_count++;
  }

  for (int b = 0; b < MAX_BUILDINGS; b++) {
    struct Building* bld = &w->buildings[b];
    if (!bld->active) continue;
    if (bld->type == BLD_TOWER) tower_count++;
    else if (bld->type == BLD_BARRACKS) barracks_count++;
  }

  /* 1. Train additional villagers if economy needs hands */
  if (villager_count < 7 && w->food >= 40 && w->population < w->max_population) {
    w->food -= 40;
    unit_spawn(w, UNIT_VILLAGER, TEAM_PLAYER, (31 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
    add_float_text(w, 31 * TILE_PX, 33 * TILE_PX, "+CITIZEN", PAL_GOLD_BRIGHT);
  }

  /* 2. Train military troops at Barracks if available */
  if (barracks_count > 0 && military_count < 8 && w->population < w->max_population) {
    if (w->food >= 50 && w->gold >= 25) {
      w->food -= 50;
      w->gold -= 25;
      int mtype = (military_count % 2 == 0) ? UNIT_SWORDSMAN : UNIT_ARCHER;
      unit_spawn(w, mtype, TEAM_PLAYER, (33 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
      add_float_text(w, 33 * TILE_PX, 33 * TILE_PX, "+KNIGHT", PAL_BANNER_BLUE);
    }
  }

  /* 3. Construct housing when nearing population capacity */
  if (w->population >= w->max_population - 2 && w->wood >= 40) {
    int bx, by;
    if (find_build_spot(w, BLD_HOUSE, &bx, &by)) {
      world_place_building(w, BLD_HOUSE, bx, by);
    }
  }

  /* 4. Construct farm if food supply is running dry */
  if (w->food < 60 && w->wood >= 30) {
    int bx, by;
    if (find_build_spot(w, BLD_FARM, &bx, &by)) {
      world_place_building(w, BLD_FARM, bx, by);
    }
  }

  /* 5. Build defensive watchtowers around kingdom */
  if (tower_count < 4 && w->wood >= 50 && w->stone >= 35) {
    int bx, by;
    if (find_build_spot(w, BLD_TOWER, &bx, &by)) {
      world_place_building(w, BLD_TOWER, bx, by);
    }
  }

  /* 6. Build barracks once base defense is established */
  if (barracks_count == 0 && w->wood >= 70 && w->stone >= 40) {
    int bx, by;
    if (find_build_spot(w, BLD_BARRACKS, &bx, &by)) {
      world_place_building(w, BLD_BARRACKS, bx, by);
    }
  }

  /* 7. Assign idle villagers to harvest necessary resources */
  for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
    struct Unit* unit = &w->units[u];
    if (!unit->active || unit->team != TEAM_PLAYER || unit->type != UNIT_VILLAGER) continue;

    if (unit->state == STATE_IDLE && unit->carry_amount == 0) {
      int wanted_res = RES_TREE;
      if (w->wood > 100 && w->stone < 60) wanted_res = RES_STONE;
      else if (w->wood > 100 && w->stone > 60 && w->gold < 50) wanted_res = RES_GOLD;

      int rx, ry;
      if (find_nearest_resource(w, unit->x >> FIXED_SHIFT >> 4, unit->y >> FIXED_SHIFT >> 4, wanted_res, &rx, &ry)) {
        unit->target_tile_x = rx;
        unit->target_tile_y = ry;
        unit->target_x = (rx * TILE_PX + 8) << FIXED_SHIFT;
        unit->target_y = (ry * TILE_PX + 8) << FIXED_SHIFT;
        unit->state = STATE_GATHER;
      }
    }
  }

  /* 8. Order military troops to engage hostile invaders */
  int enemy_target = -1;
  int min_dsq = 999999;
  for (int e = 0; e < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; e++) {
    struct Unit* enemy = &w->units[e];
    if (!enemy->active || enemy->team != TEAM_ENEMY || enemy->hp <= 0) continue;
    int ex = enemy->x >> FIXED_SHIFT;
    int ey = enemy->y >> FIXED_SHIFT;
    int dsq = (ex - (31 * TILE_PX)) * (ex - (31 * TILE_PX)) + (ey - (31 * TILE_PX)) * (ey - (31 * TILE_PX));
    if (dsq < min_dsq) {
      min_dsq = dsq;
      enemy_target = e;
    }
  }

  if (enemy_target != -1) {
    struct Unit* target = &w->units[enemy_target];
    for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
      struct Unit* soldier = &w->units[u];
      if (!soldier->active || soldier->team != TEAM_PLAYER) continue;
      if (soldier->type == UNIT_SWORDSMAN || soldier->type == UNIT_ARCHER) {
        if (soldier->state == STATE_IDLE) {
          soldier->target_x = target->x;
          soldier->target_y = target->y;
        }
      }
    }
  }
}
