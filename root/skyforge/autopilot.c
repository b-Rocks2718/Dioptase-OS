#include "skyforge.h"

/*
 * Autonomous AI Ace Wingman pilot for the Home Screen attract mode, `--demo`,
 * and in-game `D` autopilot toggle.
 */

/* Wrap a 16.16 delta into [-64*FIX_ONE, +64*FIX_ONE) on the 128x128 torus. */
static int wrap_delta_ai(int d){
  int half_map = 64 << FIX_SHIFT;
  int full_map = 128 << FIX_SHIFT;
  while (d > half_map) d -= full_map;
  while (d < -half_map) d += full_map;
  return d;
}

/* Compute one tick of autonomous AI flight, combat, or sculpting controls. */
void autopilot_decide(struct FlightControls* ctrl){
  ctrl->turn = 0;
  ctrl->pitch = 0;
  ctrl->throttle = 0;
  ctrl->fire_primary = 0;
  ctrl->fire_seismic = 0;
  ctrl->barrel_roll = 0;
  ctrl->sculpt_apply = 0;
  ctrl->sculpt_erase = 0;

  int dir_x = icos(g_player.angle);
  int dir_y = isin(g_player.angle);
  int right_x = -dir_y;
  int right_y = dir_x;

  /* 1. Check incoming enemy shots for defensive barrel-roll deflection */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    if (s->kind != SHOT_ENEMY_LASER && s->kind != SHOT_BOSS_ORB) continue;
    int dx = wrap_delta_ai(s->x - g_player.x);
    int dy = wrap_delta_ai(s->y - g_player.y);
    if (dx > -(6 << FIX_SHIFT) && dx < (6 << FIX_SHIFT) &&
        dy > -(6 << FIX_SHIFT) && dy < (6 << FIX_SHIFT)){
      ctrl->barrel_roll = 1;
      break;
    }
  }

  /* 2. In Terrain Forge mode, orbit smoothly and demonstrate live sculpting */
  if (g_mode == MODE_FORGE){
    ctrl->turn = ((g_frame & 127) < 64) ? 1 : -1;
    if ((g_frame & 31) == 0){
      g_player.forge_tool = (g_player.forge_tool + 1) % TOOL_COUNT;
      ctrl->sculpt_apply = 1;
    }
    return;
  }

  /* 3. Find the best target entity (next race gate, ring, beacon, or enemy) */
  int best_idx = -1;
  int best_score = 0x7FFFFFFF;
  for (int i = 0; i < MAX_ENTITIES; ++i){
    struct Entity* e = &g_entities[i];
    if (!e->active) continue;
    if (g_mode == MODE_RACE){
      if (e->kind != ENT_GATE || e->phase != g_player.next_gate) continue;
    } else {
      if (e->kind == ENT_ARCH || e->kind == ENT_SPIRE || e->kind == ENT_RIVAL) continue;
    }
    int dx = wrap_delta_ai(e->x - g_player.x);
    int dy = wrap_delta_ai(e->y - g_player.y);
    int dist = approx_dist2d(dx >> FIX_SHIFT, dy >> FIX_SHIFT);
    int weight = dist;
    if (e->kind == ENT_RING || e->kind == ENT_BEACON) weight -= 6;
    if (e->kind == ENT_BOSS) weight -= 10;
    if (weight < best_score){
      best_score = weight;
      best_idx = i;
    }
  }

  int desired_z = (WATER_LEVEL + 14) << FIX_SHIFT;

  if (best_idx >= 0){
    struct Entity* target = &g_entities[best_idx];
    int dx = wrap_delta_ai(target->x - g_player.x);
    int dy = wrap_delta_ai(target->y - g_player.y);
    int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    int side = fixmul(dx, right_x) + fixmul(dy, right_y);

    if (fwd < 0 || side > (2 << FIX_SHIFT)){
      ctrl->turn = 1;
    } else if (side < -(2 << FIX_SHIFT)){
      ctrl->turn = -1;
    }

    desired_z = target->z;

    /* Aim reticle and fire weapons when facing a hostile target */
    if (target->kind >= ENT_SCOUT && target->kind <= ENT_BOSS){
      int aim_tx = (side >> 12);
      if (aim_tx > 48) aim_tx = 48;
      if (aim_tx < -48) aim_tx = -48;
      g_player.aim_x = aim_tx;
      g_player.aim_y = 0;
      if (fwd > (3 << FIX_SHIFT) && fwd < (40 << FIX_SHIFT)){
        int abs_side = side < 0 ? -side : side;
        if (abs_side < (fwd >> 1)){
          ctrl->fire_primary = 1;
          if ((g_frame & 63) == 0 && fwd < (24 << FIX_SHIFT)){
            ctrl->fire_seismic = 1;
          }
        }
      }
    } else {
      /* Boost toward rings and Grand Prix gates when lined up */
      int abs_side = side < 0 ? -side : side;
      if (fwd > 0 && abs_side < (4 << FIX_SHIFT) && g_player.energy > 35){
        ctrl->throttle = 1;
      }
    }
  } else {
    /* Gentle canyon slalom when no target is selected */
    ctrl->turn = (isin((int)g_frame << 3) > 0) ? 1 : -1;
  }

  /* 4. Terrain lookahead avoidance: climb if a ridge is ahead */
  int ahead_x = g_player.x + (dir_x << 3);
  int ahead_y = g_player.y + (dir_y << 3);
  int ridge_z = terrain_height_at(ahead_x, ahead_y) + (8 << FIX_SHIFT);
  if (ridge_z > desired_z) desired_z = ridge_z;

  if (g_player.z < desired_z - (1 << FIX_SHIFT)){
    ctrl->pitch = 1;
  } else if (g_player.z > desired_z + (2 << FIX_SHIFT)){
    ctrl->pitch = -1;
  }
}
