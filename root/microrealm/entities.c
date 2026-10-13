#include "realm.h"
#include "assets.h"
#include "../crt/string.h"

/* Allocate and spawn an entity into the game universe. */
int unit_spawn(struct GameWorld* w, int type, int team, int px, int py) {
  int slot = -1;
  for (int i = 0; i < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; i++) {
    if (!w->units[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == -1) return -1;

  struct Unit* u = &w->units[slot];
  memset(u, 0, sizeof(struct Unit));
  u->active = 1;
  u->type = type;
  u->team = team;
  u->state = STATE_IDLE;
  u->x = px;
  u->y = py;
  u->target_x = px;
  u->target_y = py;
  u->target_unit = -1;
  u->target_building = -1;
  u->target_tile_x = -1;
  u->target_tile_y = -1;

  switch (type) {
    case UNIT_VILLAGER:  u->hp = 80;  u->max_hp = 80;  break;
    case UNIT_SWORDSMAN: u->hp = 180; u->max_hp = 180; break;
    case UNIT_ARCHER:    u->hp = 90;  u->max_hp = 90;  break;
    case UNIT_GOBLIN:    u->hp = 60;  u->max_hp = 60;  break;
    case UNIT_ORC:       u->hp = 140; u->max_hp = 140; break;
    case UNIT_SKELETON:  u->hp = 85;  u->max_hp = 85;  break;
    case UNIT_OGRE:      u->hp = 380; u->max_hp = 380; break;
    case UNIT_BOSS:      u->hp = 750; u->max_hp = 750; break;
    default:             u->hp = 50;  u->max_hp = 50;  break;
  }

  if (team == TEAM_PLAYER) {
    w->population++;
  }

  return slot;
}

/* Dispatch an airborne ballistic projectile toward target coordinates. */
void projectile_spawn(struct GameWorld* w, int team, int sx, int sy, int tx, int ty, int dmg) {
  int slot = -1;
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    if (!w->projectiles[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == -1) return;

  struct Projectile* p = &w->projectiles[slot];
  p->active = 1;
  p->team = team;
  p->x = sx << FIXED_SHIFT;
  p->y = sy << FIXED_SHIFT;
  p->z = 8 << FIXED_SHIFT;
  p->damage = dmg;

  int dx = tx - sx;
  int dy = ty - sy;
  int dist = (dx * dx + dy * dy);
  /* Normalize velocity */
  int speed = 7;
  if (dist > 0) {
    int approx_dist = 1;
    while (approx_dist * approx_dist < dist) approx_dist++;
    p->vx = (dx * speed * FIXED_ONE) / approx_dist;
    p->vy = (dy * speed * FIXED_ONE) / approx_dist;
  } else {
    p->vx = 0;
    p->vy = 0;
  }
  p->vz = 2 << FIXED_SHIFT;
}

/* Spawn a transient particle for dust, blood, or sparks. */
void add_particle(struct GameWorld* w, int x, int y, int vx, int vy, short color, int life) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!w->particles[i].active) {
      struct Particle* p = &w->particles[i];
      p->active = 1;
      p->x = x << FIXED_SHIFT;
      p->y = y << FIXED_SHIFT;
      p->vx = vx;
      p->vy = vy;
      p->color = color;
      p->life = life;
      return;
    }
  }
}

/* Emit a rising textual notification over the world view. */
void add_float_text(struct GameWorld* w, int x, int y, char* text, short color) {
  for (int i = 0; i < MAX_FLOAT_TEXTS; i++) {
    if (!w->float_texts[i].active) {
      struct FloatText* ft = &w->float_texts[i];
      ft->active = 1;
      ft->x = x;
      ft->y = y;
      ft->color = color;
      ft->life = 35;
      int idx = 0;
      while (text[idx] != '\0' && idx < 11) {
        ft->text[idx] = text[idx];
        idx++;
      }
      ft->text[idx] = '\0';
      return;
    }
  }
}

/* Advance positions, collisions, and lifetimes of in-flight projectiles. */
void projectiles_update(struct GameWorld* w) {
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    struct Projectile* p = &w->projectiles[i];
    if (!p->active) continue;

    p->x += p->vx;
    p->y += p->vy;
    p->z += p->vz;
    p->vz -= (FIXED_ONE / 8); /* Gravity arc */

    int px = p->x >> FIXED_SHIFT;
    int py = p->y >> FIXED_SHIFT;

    /* Check ground contact */
    if (p->z <= 0) {
      p->active = 0;
      add_particle(w, px, py, 0, 0, PAL_DIRT, 6);
      continue;
    }

    /* Check collisions against opposing units */
    for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
      struct Unit* unit = &w->units[u];
      if (!unit->active || unit->team == p->team || unit->state == STATE_DEAD) continue;

      int ux = unit->x >> FIXED_SHIFT;
      int uy = unit->y >> FIXED_SHIFT;
      int dx = px - ux;
      int dy = py - uy;
      if (dx * dx + dy * dy < (12 * 12)) {
        unit->hp -= p->damage;
        p->active = 0;
        audio_play_sfx(SFX_ARROW_SHOOT);
        add_particle(w, px, py, 2, -2, PAL_CRIMSON, 10);
        add_float_text(w, px, py - 6, "-20", PAL_CRIMSON);
        break;
      }
    }
  }
}

/* Advance particle physics and decay timers. */
void particles_update(struct GameWorld* w) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    struct Particle* p = &w->particles[i];
    if (!p->active) continue;

    p->x += p->vx << (FIXED_SHIFT - 2);
    p->y += p->vy << (FIXED_SHIFT - 2);
    p->life--;
    if (p->life <= 0) {
      p->active = 0;
    }
  }
}

/* Advance floating combat and bounty text positions. */
void float_texts_update(struct GameWorld* w) {
  for (int i = 0; i < MAX_FLOAT_TEXTS; i++) {
    struct FloatText* ft = &w->float_texts[i];
    if (!ft->active) continue;

    ft->y--;
    ft->life--;
    if (ft->life <= 0) {
      ft->active = 0;
    }
  }
}

/* Locate the nearest castle or storehouse structure to deposit gathered goods. */
static int find_nearest_depot(struct GameWorld* w, int ux, int uy) {
  int best_id = -1;
  int best_dist_sq = 99999999;
  for (int i = 0; i < MAX_BUILDINGS; i++) {
    struct Building* b = &w->buildings[i];
    if (!b->active) continue;
    if (b->type != BLD_CASTLE && b->type != BLD_LUMBER_CAMP) continue;

    int bx = (b->tile_x * TILE_PX) + (b->width * TILE_PX / 2);
    int by = (b->tile_y * TILE_PX) + (b->height * TILE_PX / 2);
    int dsq = (bx - ux) * (bx - ux) + (by - uy) * (by - uy);
    if (dsq < best_dist_sq) {
      best_dist_sq = dsq;
      best_id = i;
    }
  }
  return best_id;
}

/* Step entity intelligence, pathing, combat, and resource extraction. */
void units_update(struct GameWorld* w) {
  for (int i = 0; i < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; i++) {
    struct Unit* u = &w->units[i];
    if (!u->active) continue;

    /* Handle fallen entity */
    if (u->hp <= 0) {
      if (u->team == TEAM_PLAYER) {
        w->population--;
        add_float_text(w, u->x >> FIXED_SHIFT, u->y >> FIXED_SHIFT, "FALLEN", PAL_CRIMSON);
      } else {
        w->enemies_slain++;
        int bounty = 40;
        if (u->type == UNIT_ORC) bounty = 80;
        else if (u->type == UNIT_SKELETON) bounty = 100;
        else if (u->type == UNIT_OGRE) bounty = 250;
        else if (u->type == UNIT_BOSS) bounty = 1000;
        w->score += bounty;
        w->gold += (bounty / 20);
        add_float_text(w, u->x >> FIXED_SHIFT, u->y >> FIXED_SHIFT, "+SCORE", PAL_GOLD_BRIGHT);
      }
      u->active = 0;
      continue;
    }

    if (u->cooldown > 0) u->cooldown--;

    int ux = u->x >> FIXED_SHIFT;
    int uy = u->y >> FIXED_SHIFT;

    /* Movement step toward target coordinates */
    int t_px = u->target_x >> FIXED_SHIFT;
    int t_py = u->target_y >> FIXED_SHIFT;
    int dx = t_px - ux;
    int dy = t_py - uy;
    int dist_sq = dx * dx + dy * dy;

    int move_speed = (u->type == UNIT_GOBLIN) ? 384 : (u->type == UNIT_OGRE ? 180 : 256);

    if (dist_sq > 16) {
      u->state = STATE_WALK;
      u->anim_timer++;
      if (u->anim_timer > 6) {
        u->anim_timer = 0;
        u->anim_frame ^= 1;
      }
      if (dx < 0) u->facing_left = 1;
      else if (dx > 0) u->facing_left = 0;

      int step_x = 0;
      int step_y = 0;
      if (dx > 0) step_x = (dx > 2) ? move_speed : (dx << FIXED_SHIFT);
      else if (dx < 0) step_x = (dx < -2) ? -move_speed : (dx << FIXED_SHIFT);
      if (dy > 0) step_y = (dy > 2) ? move_speed : (dy << FIXED_SHIFT);
      else if (dy < 0) step_y = (dy < -2) ? -move_speed : (dy << FIXED_SHIFT);

      u->x += step_x;
      u->y += step_y;
    } else {
      if (u->state == STATE_WALK) {
        u->state = STATE_IDLE;
      }
    }

    /* Villager resource gathering and haulage */
    if (u->type == UNIT_VILLAGER) {
      if (u->carry_amount >= 10) {
        /* Head to depot */
        int depot_id = find_nearest_depot(w, ux, uy);
        if (depot_id != -1) {
          struct Building* b = &w->buildings[depot_id];
          int bx = (b->tile_x * TILE_PX) + (b->width * TILE_PX / 2);
          int by = (b->tile_y * TILE_PX) + (b->height * TILE_PX / 2);
          u->target_x = bx << FIXED_SHIFT;
          u->target_y = by << FIXED_SHIFT;

          int d_depot_sq = (bx - ux) * (bx - ux) + (by - uy) * (by - uy);
          if (d_depot_sq < (28 * 28)) {
            if (u->carry_type == RES_TREE) {
              w->wood += u->carry_amount;
              add_float_text(w, bx, by - 12, "+10 WOOD", PAL_GRASS_LIGHT);
              audio_play_sfx(SFX_WOOD_CHOP);
            } else if (u->carry_type == RES_STONE) {
              w->stone += u->carry_amount;
              add_float_text(w, bx, by - 12, "+10 STONE", PAL_STONE_LGT);
              audio_play_sfx(SFX_STONE_MINE);
            } else if (u->carry_type == RES_GOLD) {
              w->gold += u->carry_amount;
              add_float_text(w, bx, by - 12, "+10 GOLD", PAL_GOLD_BRIGHT);
              audio_play_sfx(SFX_GOLD_CHIME);
            }
            u->carry_amount = 0;
            u->carry_type = RES_NONE;
            /* Resume gathering previous node if intact */
            if (u->target_tile_x >= 0 && u->target_tile_y >= 0) {
              u->target_x = (u->target_tile_x * TILE_PX + 8) << FIXED_SHIFT;
              u->target_y = (u->target_tile_y * TILE_PX + 8) << FIXED_SHIFT;
              u->state = STATE_GATHER;
            } else {
              u->state = STATE_IDLE;
            }
          }
        }
      } else if (u->state == STATE_GATHER) {
        if (u->target_tile_x >= 0 && u->target_tile_y >= 0) {
          struct MapCell* cell = world_get_cell(w, u->target_tile_x, u->target_tile_y);
          if (cell != NULL && cell->resource != RES_NONE) {
            int rx = u->target_tile_x * TILE_PX + 8;
            int ry = u->target_tile_y * TILE_PX + 8;
            int d_res_sq = (rx - ux) * (rx - ux) + (ry - uy) * (ry - uy);
            if (d_res_sq < (24 * 24)) {
              if (u->cooldown == 0) {
                u->cooldown = 18;
                u->carry_type = cell->resource;
                u->carry_amount += 2;
                cell->resource_hp -= 15;
                if (cell->resource == RES_TREE) {
                  audio_play_sfx(SFX_WOOD_CHOP);
                  add_particle(w, rx, ry, 2, -2, PAL_WOOD_BARK, 6);
                } else {
                  audio_play_sfx(SFX_STONE_MINE);
                  add_particle(w, rx, ry, 2, -2, PAL_STONE_LGT, 6);
                }
                if (cell->resource_hp <= 0) {
                  cell->resource = RES_NONE;
                  cell->resource_hp = 0;
                }
              }
            }
          } else {
            u->state = STATE_IDLE;
          }
        }
      }
    }

    /* Military combat logic */
    if (u->team == TEAM_PLAYER && (u->type == UNIT_SWORDSMAN || u->type == UNIT_ARCHER)) {
      /* Acquire nearest enemy within detection radius */
      int nearest_enemy = -1;
      int best_dsq = (110 * 110);
      for (int e = 0; e < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; e++) {
        struct Unit* enemy = &w->units[e];
        if (!enemy->active || enemy->team != TEAM_ENEMY || enemy->hp <= 0) continue;
        int ex = enemy->x >> FIXED_SHIFT;
        int ey = enemy->y >> FIXED_SHIFT;
        int dsq = (ex - ux) * (ex - ux) + (ey - uy) * (ey - uy);
        if (dsq < best_dsq) {
          best_dsq = dsq;
          nearest_enemy = e;
        }
      }

      if (nearest_enemy != -1) {
        struct Unit* target = &w->units[nearest_enemy];
        int tx = target->x >> FIXED_SHIFT;
        int ty = target->y >> FIXED_SHIFT;

        if (u->type == UNIT_SWORDSMAN) {
          if (best_dsq < (20 * 20)) {
            if (u->cooldown == 0) {
              u->cooldown = 14;
              target->hp -= 28;
              audio_play_sfx(SFX_SWORD_HIT);
              add_particle(w, tx, ty, 3, -2, PAL_STEEL_BRIGHT, 8);
              add_float_text(w, tx, ty - 6, "-28", PAL_CRIMSON);
            }
          } else {
            u->target_x = target->x;
            u->target_y = target->y;
          }
        } else if (u->type == UNIT_ARCHER) {
          if (best_dsq < (80 * 80)) {
            if (u->cooldown == 0) {
              u->cooldown = 24;
              projectile_spawn(w, TEAM_PLAYER, ux, uy, tx, ty, 22);
              audio_play_sfx(SFX_ARROW_SHOOT);
            }
          } else {
            u->target_x = target->x;
            u->target_y = target->y;
          }
        }
      }
    }

    /* Hostile monster combat logic */
    if (u->team == TEAM_ENEMY) {
      /* Target player units or buildings */
      int nearest_target = -1;
      int best_dsq = (150 * 150);
      for (int p = 0; p < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; p++) {
        struct Unit* hero = &w->units[p];
        if (!hero->active || hero->team != TEAM_PLAYER || hero->hp <= 0) continue;
        int hx = hero->x >> FIXED_SHIFT;
        int hy = hero->y >> FIXED_SHIFT;
        int dsq = (hx - ux) * (hx - ux) + (hy - uy) * (hy - uy);
        if (dsq < best_dsq) {
          best_dsq = dsq;
          nearest_target = p;
        }
      }

      if (nearest_target != -1) {
        struct Unit* target = &w->units[nearest_target];
        int tx = target->x >> FIXED_SHIFT;
        int ty = target->y >> FIXED_SHIFT;

        if (u->type == UNIT_SKELETON) {
          if (best_dsq < (80 * 80)) {
            if (u->cooldown == 0) {
              u->cooldown = 28;
              projectile_spawn(w, TEAM_ENEMY, ux, uy, tx, ty, 16);
            }
          } else {
            u->target_x = target->x;
            u->target_y = target->y;
          }
        } else {
          /* Melee hostiles */
          if (best_dsq < (20 * 20)) {
            if (u->cooldown == 0) {
              u->cooldown = 16;
              int dmg = (u->type == UNIT_GOBLIN) ? 12 : (u->type == UNIT_ORC ? 22 : (u->type == UNIT_OGRE ? 38 : 55));
              target->hp -= dmg;
              audio_play_sfx(SFX_SWORD_HIT);
              add_particle(w, tx, ty, -2, -2, PAL_CRIMSON, 8);
              add_float_text(w, tx, ty - 6, "-HIT", PAL_CRIMSON);
            }
          } else {
            u->target_x = target->x;
            u->target_y = target->y;
          }
        }
      } else {
        /* March toward Castle */
        int castle_id = -1;
        for (int b = 0; b < MAX_BUILDINGS; b++) {
          if (w->buildings[b].active && w->buildings[b].type == BLD_CASTLE) {
            castle_id = b;
            break;
          }
        }
        if (castle_id != -1) {
          struct Building* cb = &w->buildings[castle_id];
          int cx = (cb->tile_x * TILE_PX) + (cb->width * TILE_PX / 2);
          int cy = (cb->tile_y * TILE_PX) + (cb->height * TILE_PX / 2);
          int d_castle = (cx - ux) * (cx - ux) + (cy - uy) * (cy - uy);
          if (d_castle < (30 * 30)) {
            if (u->cooldown == 0) {
              u->cooldown = 20;
              cb->hp -= 20;
              audio_play_sfx(SFX_SWORD_HIT);
              add_particle(w, cx, cy, 0, -2, PAL_STONE_LGT, 10);
            }
          } else {
            u->target_x = cx << FIXED_SHIFT;
            u->target_y = cy << FIXED_SHIFT;
          }
        }
      }
    }
  }
}
