#include "skyforge.h"

/*
 * Autonomous AI Ace Wingman pilot for the Home Screen attract mode, `--demo`,
 * and in-game `D` autopilot toggle. Follows the 3-phase Campaign objectives
 * (Shield Towers -> Sector Guardian -> Emerald Warp Portal) and Grand Prix gates.
 */

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
  ctrl->strafe = 0;
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

  /* 1. Barrel-roll deflection when an enemy laser or boss orb approaches */
  for (int i = 0; i < MAX_SHOTS; ++i){
    struct Shot* s = &g_shots[i];
    if (!s->active) continue;
    if (s->kind != SHOT_ENEMY_LASER && s->kind != SHOT_BOSS_ORB) continue;
    int dx = wrap_delta_ai(s->x - g_player.x);
    int dy = wrap_delta_ai(s->y - g_player.y);
    if (dx > -(5 << FIX_SHIFT) && dx < (5 << FIX_SHIFT) &&
        dy > -(5 << FIX_SHIFT) && dy < (5 << FIX_SHIFT)){
      ctrl->barrel_roll = 1;
      break;
    }
  }

  /* 2. In Terrain Forge mode, orbit smoothly and demonstrate live sculpting */
  if (g_mode == MODE_FORGE){
    ctrl->turn = ((g_frame & 127) < 64) ? 6 : -6;
    if ((g_frame & 31) == 0){
      g_player.forge_tool = (g_player.forge_tool + 1) % TOOL_COUNT;
      ctrl->sculpt_apply = 1;
    }
    return;
  }

  /* 3. Prioritize current phase objective (Tower, Boss, Portal, or Gate) */
  int best_idx = g_player.nav_target;
  if (best_idx < 0 || !g_entities[best_idx].active){
    best_idx = g_player.lock_target;
  }

  int desired_z = (WATER_LEVEL + 12) << FIX_SHIFT;

  if (best_idx >= 0 && g_entities[best_idx].active){
    struct Entity* target = &g_entities[best_idx];
    int dx = wrap_delta_ai(target->x - g_player.x);
    int dy = wrap_delta_ai(target->y - g_player.y);
    int fwd = fixmul(dx, dir_x) + fixmul(dy, dir_y);
    int side = fixmul(dx, right_x) + fixmul(dy, right_y);

    if (fwd < 0){
      ctrl->turn = (side >= 0) ? 10 : -10;
    } else if (side > (1 << FIX_SHIFT)){
      ctrl->turn = 6;
    } else if (side < -(1 << FIX_SHIFT)){
      ctrl->turn = -6;
    }

    desired_z = target->z;

    if (target->kind == ENT_GENERATOR ||
        (target->kind >= ENT_SCOUT && target->kind <= ENT_BOSS)){
      int aim_tx = side >> 12;
      if (aim_tx > 64) aim_tx = 64;
      if (aim_tx < -64) aim_tx = -64;
      g_player.aim_x = aim_tx;
      g_player.aim_y = 0;
      if (fwd > (2 << FIX_SHIFT) && fwd < (42 << FIX_SHIFT)){
        ctrl->fire_primary = 1;
        if ((g_frame & 63) == 0 && fwd < (24 << FIX_SHIFT)){
          ctrl->fire_seismic = 1;
        }
      }
    } else {
      g_player.aim_x = 0;
      g_player.aim_y = 0;
      if (fwd > 0 && g_player.energy > 35){
        ctrl->throttle = 1;
      }
    }
  } else {
    /* Steer along the canyon center north (-Y, angle 768) */
    int angle_err = (768 - g_player.angle) & ANGLE_MASK;
    if (angle_err > 512) angle_err -= 1024;
    if (angle_err > 8) ctrl->turn = 6;
    else if (angle_err < -8) ctrl->turn = -6;
    ctrl->fire_primary = (g_player.lock_target >= 0);
  }

  if (g_player.z < desired_z - (1 << FIX_SHIFT)){
    ctrl->pitch = 4;
  } else if (g_player.z > desired_z + (2 << FIX_SHIFT)){
    ctrl->pitch = -4;
  }
}
