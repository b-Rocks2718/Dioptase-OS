/* Emberfall pure rules: movement, combat, spawning, XP, and upgrades.
 *
 * No syscalls or display/audio access. All positions are 8.8 fixed point.
 * The tick runs at 30 Hz; cooldowns and timers count ticks. Enemies seek
 * with an axis-scaled step that needs one division and no square root.
 * Range checks compare squared pixel distances.
 */

#include "emberfall.h"

/* Balance: spawn pacing, timers, and arena margins. */
#define START_HP 100
#define BASE_SPEED 512
#define BASE_MAGNET 24
#define COLLECT_DIST 8
#define INVULN_TICKS 15
#define ARENA_MARGIN 10
#define BOSS_EVERY_TICKS 2700
#define MAX_LEVEL_CHOICES 3

/* Advance the xorshift32 state and return the new word. */
static unsigned next_rand(struct Game* game) {
  unsigned v = game->rng;
  v ^= v << 13;
  v ^= v >> 17;
  v ^= v << 5;
  game->rng = v;
  return v;
}

/* Integer absolute value. */
static int abs_int(int v) {
  return v < 0 ? -v : v;
}

/* Base max HP by enemy kind. */
static int enemy_base_hp(int kind) {
  if (kind == ENEMY_SLIME) return 10;
  if (kind == ENEMY_BAT) return 6;
  if (kind == ENEMY_SKELETON) return 20;
  if (kind == ENEMY_ORC) return 40;
  if (kind == ENEMY_WRAITH) return 30;
  if (kind == ENEMY_GOLEM) return 100;
  return 500;
}

/* Base speed in fixed units per tick by kind. */
static int enemy_base_speed(int kind) {
  if (kind == ENEMY_SLIME) return 128;
  if (kind == ENEMY_BAT) return 307;
  if (kind == ENEMY_SKELETON) return 205;
  if (kind == ENEMY_ORC) return 154;
  if (kind == ENEMY_WRAITH) return 256;
  if (kind == ENEMY_GOLEM) return 102;
  return 128;
}

/* Base contact damage by kind. */
static int enemy_base_damage(int kind) {
  if (kind == ENEMY_SLIME) return 8;
  if (kind == ENEMY_BAT) return 6;
  if (kind == ENEMY_SKELETON) return 12;
  if (kind == ENEMY_ORC) return 15;
  if (kind == ENEMY_WRAITH) return 18;
  if (kind == ENEMY_GOLEM) return 25;
  return 30;
}

/* XP value dropped by kind. */
static int enemy_xp(int kind) {
  if (kind == ENEMY_SLIME) return 1;
  if (kind == ENEMY_BAT) return 1;
  if (kind == ENEMY_SKELETON) return 2;
  if (kind == ENEMY_ORC) return 3;
  if (kind == ENEMY_WRAITH) return 4;
  if (kind == ENEMY_GOLEM) return 8;
  return 20;
}

/* Score awarded by kind. */
static int enemy_score(int kind) {
  if (kind == ENEMY_SLIME) return 10;
  if (kind == ENEMY_BAT) return 15;
  if (kind == ENEMY_SKELETON) return 25;
  if (kind == ENEMY_ORC) return 40;
  if (kind == ENEMY_WRAITH) return 60;
  if (kind == ENEMY_GOLEM) return 100;
  return 500;
}

/* Body radius in pixels by kind, for collisions and drawing. */
static int enemy_radius(int kind) {
  if (kind == ENEMY_BAT) return 5;
  if (kind == ENEMY_GOLEM) return 10;
  if (kind == ENEMY_BOSS) return 14;
  return 7;
}

/* Unlock second for each normal kind. Bosses use the boss timer. */
static int enemy_unlock_sec(int kind) {
  if (kind == ENEMY_SLIME) return 0;
  if (kind == ENEMY_BAT) return 30;
  if (kind == ENEMY_SKELETON) return 60;
  if (kind == ENEMY_ORC) return 120;
  if (kind == ENEMY_WRAITH) return 180;
  if (kind == ENEMY_GOLEM) return 240;
  return 1000000;
}

/* Player move speed including Swift stacks, in fixed units per tick. */
static int player_speed(struct Player* p) {
  int swift = p->passive_level[PASSIVE_SWIFT];
  return BASE_SPEED * (100 + swift * 8) / 100;
}

/* Damage multiplier percent including Power stacks. */
static int damage_percent(struct Player* p) {
  return 100 + p->passive_level[PASSIVE_POWER] * 12;
}

/* Magnet radius in pixels including Magnet stacks. */
static int magnet_radius(struct Player* p) {
  int m = p->passive_level[PASSIVE_MAGNET];
  return BASE_MAGNET * (100 + m * 25) / 100;
}

/* XP gain percent including Tome stacks. */
static int xp_percent(struct Player* p) {
  return 100 + p->passive_level[PASSIVE_TOME] * 12;
}

/* XP needed for the next level. */
static int xp_threshold(int level) {
  return 8 + level * 6;
}

/* Spawn interval in ticks from survived seconds. */
static int spawn_interval(int sec) {
  int v = 30 - sec / 4;
  if (v < 5) v = 5;
  return v;
}

/* How many to spawn per event from survived seconds. */
static int spawn_batch(int sec) {
  if (sec > 240) return 4;
  if (sec > 120) return 3;
  if (sec > 45) return 2;
  return 1;
}

/* Squared pixel distance between two fixed points. */
static int dist2_fixed(int x1, int y1, int x2, int y2) {
  int dx = (x1 - x2) >> FIXED_SHIFT;
  int dy = (y1 - y2) >> FIXED_SHIFT;
  return dx * dx + dy * dy;
}

/* Find a free enemy slot, or 0 when full. Pointers avoid the slow
 * index-scale multiply bcc emits for struct array subscripting. */
static struct Enemy* free_enemy(struct Game* game) {
  struct Enemy* e = game->enemies;
  int i = 0;
  while (i < 48) {
    if (!e->active) return e;
    e++;
    i++;
  }
  return 0;
}

/* Find a free projectile slot, or 0 when full. */
static struct Projectile* free_projectile(struct Game* game) {
  struct Projectile* p = game->projectiles;
  int i = 0;
  while (i < 64) {
    if (!p->active) return p;
    p++;
    i++;
  }
  return 0;
}

/* Find a free gem slot, or 0 when full. */
static struct Gem* free_gem(struct Game* game) {
  struct Gem* g = game->gems;
  int i = 0;
  while (i < 64) {
    if (!g->active) return g;
    g++;
    i++;
  }
  return 0;
}

/* Find a free particle slot, or 0 when full. */
static struct Particle* free_particle(struct Game* game) {
  struct Particle* p = game->particles;
  int i = 0;
  while (i < 96) {
    if (!p->active) return p;
    p++;
    i++;
  }
  return 0;
}

/* Spawn one particle burst helper. */
static void spawn_particle(struct Game* game, int x, int y, int vx, int vy,
    int life, int color) {
  struct Particle* p = free_particle(game);
  if (!p) return;
  p->active = 1;
  p->x = x;
  p->y = y;
  p->vx = vx;
  p->vy = vy;
  p->life = life;
  p->max_life = life;
  p->color = color;
}

/* Scale one burst velocity by the speed bucket with shifts only. Shifts
 * run on the absolute value (left-shifting negatives is undefined) and
 * land within a pixel per tick of the old multiply/divide. */
static int burst_scale(int v, int mode) {
  int a = v < 0 ? -v : v;
  int m = a + (a >> 1);
  if (mode == 3) m = (a << 1) + (a >> 1);
  else if (mode == 2) m = a << 1;
  else if (mode == 1) m = a + (a >> 1) + (a >> 2);
  return v < 0 ? -m : m;
}

/* Burst n sparks around a point with random velocities. Speed is bucketed
 * into shift-add scalings (384:1.5x 448:1.75x 512:2x 640+:2.5x) because
 * bcc lowers multiply and divide to slow software routines. */
static void burst(struct Game* game, int x, int y, int n, int speed,
    int life, int color) {
  int k = 0;
  int mode = 0;
  if (speed >= 640) mode = 3;
  else if (speed >= 512) mode = 2;
  else if (speed >= 448) mode = 1;
  while (k < n) {
    int vx = (int)(next_rand(game) & 511) - 256;
    int vy = (int)(next_rand(game) & 511) - 256;
    vx = burst_scale(vx, mode);
    vy = burst_scale(vy, mode);
    spawn_particle(game, x, y, vx, vy, life, color);
    k++;
  }
}

/* Drop an XP gem with a small random scatter velocity. */
static void drop_gem(struct Game* game, int x, int y, int value) {
  struct Gem* g = free_gem(game);
  if (!g) return;
  g->active = 1;
  g->x = x;
  g->y = y;
  g->vx = (int)(next_rand(game) & 255) - 128;
  g->vy = (int)(next_rand(game) & 255) - 128;
  g->value = value;
}

/* Pick a spawn kind from unlocked types weighted toward weak foes. */
static int pick_spawn_kind(struct Game* game, int sec) {
  int pool = 0;
  int k = 0;
  int roll = 0;
  int acc = 0;
  while (k < 6) {
    if (sec >= enemy_unlock_sec(k)) pool++;
    k++;
  }
  if (pool <= 0) return ENEMY_SLIME;
  roll = (int)(next_rand(game) % 100);
  /* Early kinds stay common: slime 40, bat 25, skeleton 15, orc 10, wraith 6,
   * golem 4, restricted to unlocked pool. */
  k = 0;
  while (k < pool) {
    int w = 4;
    if (k == 0) w = 40;
    else if (k == 1) w = 25;
    else if (k == 2) w = 15;
    else if (k == 3) w = 10;
    else if (k == 4) w = 6;
    acc += w;
    if (roll < acc) return k;
    k++;
  }
  return pool - 1;
}

/* Spawn one enemy at an edge away from the player when possible. Edge
 * positions use mask-and-fold instead of modulo, and shifts instead of
 * fixed-point scaling, to avoid software multiply/divide. */
static void spawn_enemy(struct Game* game, int kind) {
  struct Enemy* e = free_enemy(game);
  int edge = 0;
  int x = 0;
  int y = 0;
  int sec = 0;
  int hp = 0;
  int dmg = 0;
  int t = 0;
  if (!e) return;
  edge = (int)(next_rand(game) & 3);
  if (edge == 0) {
    x = ARENA_MARGIN << FIXED_SHIFT;
    t = (int)(next_rand(game) & 255);
    if (t >= 236) t -= 236;
    y = t << FIXED_SHIFT;
  } else if (edge == 1) {
    x = (320 - ARENA_MARGIN) << FIXED_SHIFT;
    t = (int)(next_rand(game) & 255);
    if (t >= 236) t -= 236;
    y = t << FIXED_SHIFT;
  } else if (edge == 2) {
    t = (int)(next_rand(game) & 511);
    if (t >= 320) t -= 320;
    x = t << FIXED_SHIFT;
    y = ARENA_MARGIN << FIXED_SHIFT;
  } else {
    t = (int)(next_rand(game) & 511);
    if (t >= 320) t -= 320;
    x = t << FIXED_SHIFT;
    y = (236 - ARENA_MARGIN) << FIXED_SHIFT;
  }
  /* Keep fairness: if the edge is on top of the warden, mirror it. */
  if (dist2_fixed(x, y, game->player.x, game->player.y) < 3600) {
    x = (320 << FIXED_SHIFT) - x;
    y = (236 << FIXED_SHIFT) - y;
  }
  sec = game_seconds(game);
  hp = enemy_base_hp(kind) * (100 + (sec >> 1)) / 100;
  dmg = enemy_base_damage(kind) * (100 + (sec >> 2)) / 100;
  e->active = 1;
  e->kind = kind;
  e->x = x;
  e->y = y;
  e->hp = hp;
  e->max_hp = hp;
  e->speed = enemy_base_speed(kind);
  e->damage = dmg;
  e->slow_timer = 0;
  e->hit_timer = 0;
}

/* Spawn the periodic boss at a random edge. */
static void spawn_boss(struct Game* game) {
  spawn_enemy(game, ENEMY_BOSS);
  game->sound |= SOUND_BOSS;
  game->shake_timer = 20;
}

/* Nearest live enemy within max_d2, or 0 when none. */
static struct Enemy* nearest_enemy(struct Game* game, int x, int y,
    int max_d2) {
  struct Enemy* e = game->enemies;
  struct Enemy* best = 0;
  int best_d2 = max_d2 + 1;
  int i = 0;
  while (i < 48) {
    if (e->active) {
      int d2 = dist2_fixed(x, y, e->x, e->y);
      if (d2 <= max_d2 && d2 < best_d2) {
        best_d2 = d2;
        best = e;
      }
    }
    e++;
    i++;
  }
  return best;
}

/* Minor-axis speed bucket: approximate speed*minor/major with shifts only.
 * minor < major here, so the ratio lands in (0,1). */
static int seek_minor(int speed, int minor, int major) {
  if ((minor << 1) > major) return speed - (speed >> 2);
  if ((minor << 2) > major) return speed >> 1;
  return speed >> 2;
}

/* Fire one bolt-like projectile toward a target, returning its slot. */
static struct Projectile* fire_at(struct Game* game, int kind, int tx, int ty,
    int speed, int damage, int pierce, int life) {
  struct Projectile* p = free_projectile(game);
  int dx = 0;
  int dy = 0;
  int adx = 0;
  int ady = 0;
  int vx = 0;
  int vy = 0;
  if (!p) return 0;
  dx = tx - game->player.x;
  dy = ty - game->player.y;
  adx = abs_int(dx);
  ady = abs_int(dy);
  if (adx + ady == 0) {
    vx = speed;
    vy = 0;
  } else if (adx > ady) {
    int mag = seek_minor(speed, ady, adx);
    vx = dx > 0 ? speed : -speed;
    if (dy > 0) vy = mag;
    else if (dy < 0) vy = -mag;
  } else {
    int mag = seek_minor(speed, adx, ady);
    vy = dy > 0 ? speed : -speed;
    if (dx > 0) vx = mag;
    else if (dx < 0) vx = -mag;
  }
  p->active = 1;
  p->kind = kind;
  p->x = game->player.x;
  p->y = game->player.y;
  p->vx = vx;
  p->vy = vy;
  p->damage = damage;
  p->pierce = pierce;
  p->life = life;
  return p;
}

/* Ember Bolt: fast single bolts, more at higher levels. */
static void fire_ember(struct Game* game, int level, int pct) {
  int count = 1;
  int dmg = 0;
  int n = 0;
  if (level >= 5) count = 3;
  else if (level >= 3) count = 2;
  dmg = (10 + (level - 1) * 4) * pct / 100;
  while (n < count) {
    struct Enemy* t = nearest_enemy(game, game->player.x, game->player.y,
      40000);
    struct Projectile* slot = 0;
    if (!t) return;
    slot = fire_at(game, SPELL_EMBER, t->x, t->y, 1280, dmg, 0, 90);
    /* Fan extra bolts so each level visibly adds streams. */
    if (slot) {
      if (n == 1) slot->vy += 160;
      if (n == 2) slot->vy -= 160;
    }
    n++;
  }
  game->sound |= SOUND_SHOOT;
}

/* Nearest live enemy excluding one, within max_d2, or 0 when none. */
static struct Enemy* nearest_enemy_except(struct Game* game, int x, int y,
    int max_d2, struct Enemy* skip) {
  struct Enemy* e = game->enemies;
  struct Enemy* best = 0;
  int best_d2 = max_d2 + 1;
  int i = 0;
  while (i < 48) {
    if (e != skip && e->active) {
      int d2 = dist2_fixed(x, y, e->x, e->y);
      if (d2 <= max_d2 && d2 < best_d2) {
        best_d2 = d2;
        best = e;
      }
    }
    e++;
    i++;
  }
  return best;
}

/* Fireball: slow heavy orb with a splash on impact. */
static void fire_fireball(struct Game* game, int level, int pct) {
  int dmg = (20 + (level - 1) * 8) * pct / 100;
  struct Enemy* t = nearest_enemy(game, game->player.x, game->player.y,
    40000);
  if (!t) return;
  fire_at(game, SPELL_FIREBALL, t->x, t->y, 768, dmg, 0, 120);
  if (level >= 4) {
    struct Enemy* t2 = nearest_enemy_except(game, game->player.x,
      game->player.y, 40000, t);
    if (t2) {
      fire_at(game, SPELL_FIREBALL, t2->x, t2->y, 768, dmg, 0, 120);
    }
  }
  game->sound |= SOUND_SHOOT;
}

/* Frost Shard: quick shards that slow what they touch. */
static void fire_frost(struct Game* game, int level, int pct) {
  int count = 2;
  int dmg = (6 + (level - 1) * 3) * pct / 100;
  int n = 0;
  if (level >= 5) count = 4;
  else if (level >= 3) count = 3;
  while (n < count) {
    struct Enemy* t = nearest_enemy(game, game->player.x, game->player.y,
      32400);
    struct Projectile* slot = 0;
    if (!t) return;
    slot = fire_at(game, SPELL_FROST, t->x, t->y, 1024, dmg, 0, 80);
    /* Fan shards so higher levels spray instead of stacking. */
    if (slot) {
      if (n == 1) slot->vy += 160;
      else if (n == 2) slot->vy -= 160;
      else if (n == 3) slot->vx += 160;
    }
    n++;
  }
  game->sound |= SOUND_SHOOT;
}

/* Lightning Arc: instant chain across nearby foes. */
static void fire_arc(struct Game* game, int level, int pct) {
  int chains = 3;
  int dmg = (12 + (level - 1) * 5) * pct / 100;
  int sx = game->player.x;
  int sy = game->player.y;
  int hit_mask0 = 0;
  int hit_mask1 = 0;
  int n = 0;
  if (level >= 5) chains = 5;
  else if (level >= 3) chains = 4;
  while (n < chains) {
    int best = -1;
    int best_d2 = 6401;
    struct Enemy* e = game->enemies;
    struct Enemy* best_e = 0;
    int i = 0;
    int d2 = 0;
    while (i < 48) {
      int already = 0;
      if (i < 32) already = (hit_mask0 >> i) & 1;
      else already = (hit_mask1 >> (i - 32)) & 1;
      if (e->active && !already) {
        d2 = dist2_fixed(sx, sy, e->x, e->y);
        if (d2 < best_d2) {
          best_d2 = d2;
          best = i;
          best_e = e;
        }
      }
      e++;
      i++;
    }
    if (best < 0) break;
    if (best < 32) hit_mask0 |= 1 << best;
    else hit_mask1 |= 1 << (best - 32);
    best_e->hp -= dmg;
    best_e->hit_timer = 4;
    burst(game, best_e->x, best_e->y, 4, 384, 10, 0x0FF);
    sx = best_e->x;
    sy = best_e->y;
    n++;
  }
  if (n > 0) game->sound |= SOUND_HIT;
}

/* Holy Nova: instant ring around the warden. */
static void fire_nova(struct Game* game, int level, int pct) {
  int radius = 40 + (level - 1) * 8;
  int dmg = (15 + (level - 1) * 7) * pct / 100;
  int r2 = radius * radius;
  struct Enemy* e = game->enemies;
  int i = 0;
  int hit = 0;
  while (i < 48) {
    if (e->active) {
      int d2 = dist2_fixed(game->player.x, game->player.y, e->x, e->y);
      if (d2 <= r2) {
        e->hp -= dmg;
        e->hit_timer = 4;
        hit = 1;
      }
    }
    e++;
    i++;
  }
  burst(game, game->player.x, game->player.y, 16, 512, 14, 0xFF0);
  if (hit) game->sound |= SOUND_EXPLODE;
}

/* Cooldown in ticks for a spell at a level. */
static int spell_cooldown(int kind, int level) {
  if (kind == SPELL_EMBER) {
    int c = 15 - (level - 1);
    if (c < 8) c = 8;
    return c;
  }
  if (kind == SPELL_FIREBALL) {
    int c = 45 - (level - 1) * 3;
    if (c < 30) c = 30;
    return c;
  }
  if (kind == SPELL_FROST) {
    int c = 25 - (level - 1);
    if (c < 18) c = 18;
    return c;
  }
  if (kind == SPELL_ARC) {
    int c = 60 - (level - 1) * 5;
    if (c < 35) c = 35;
    return c;
  }
  {
    int c = 90 - (level - 1) * 8;
    if (c < 55) c = 55;
    return c;
  }
}

/* Tick all owned spells, firing those whose cooldown reached zero. */
static void update_spells(struct Game* game) {
  int pct = damage_percent(&game->player);
  int s = 0;
  while (s < 5) {
    int lvl = game->player.spell_level[s];
    if (lvl > 0) {
      game->player.cooldown[s]--;
      if (game->player.cooldown[s] <= 0) {
        if (s == SPELL_EMBER) fire_ember(game, lvl, pct);
        else if (s == SPELL_FIREBALL) fire_fireball(game, lvl, pct);
        else if (s == SPELL_FROST) fire_frost(game, lvl, pct);
        else if (s == SPELL_ARC) fire_arc(game, lvl, pct);
        else fire_nova(game, lvl, pct);
        game->player.cooldown[s] = spell_cooldown(s, lvl);
      }
    }
    s++;
  }
}

/* Move the warden with diagonal normalization and arena clamping. */
/* Diagonal speed scale (~181/256) with shifts only. Each shift floors,
 * so diagonals run a hair under true speed; imperceptible in play. */
static int diag_scale(int v) {
  int a = v < 0 ? -v : v;
  int m = (a >> 1) + (a >> 3) + (a >> 4) + (a >> 6) + (a >> 8);
  return v < 0 ? -m : m;
}

static void update_player(struct Game* game, struct GameInput input) {
  int speed = player_speed(&game->player);
  int vx = 0;
  int vy = 0;
  if (input.move_x > 0) vx = speed;
  else if (input.move_x < 0) vx = -speed;
  if (input.move_y > 0) vy = speed;
  else if (input.move_y < 0) vy = -speed;
  if (vx != 0 && vy != 0) {
    vx = diag_scale(vx);
    vy = diag_scale(vy);
  }
  if (input.move_x != 0) game->player.face_x = input.move_x;
  if (input.move_y != 0) game->player.face_y = input.move_y;
  game->player.x += vx;
  game->player.y += vy;
  if (game->player.x < (ARENA_MARGIN << FIXED_SHIFT))
    game->player.x = ARENA_MARGIN << FIXED_SHIFT;
  if (game->player.x > ((320 - ARENA_MARGIN) << FIXED_SHIFT))
    game->player.x = (320 - ARENA_MARGIN) << FIXED_SHIFT;
  if (game->player.y < (ARENA_MARGIN << FIXED_SHIFT))
    game->player.y = ARENA_MARGIN << FIXED_SHIFT;
  if (game->player.y > ((236 - ARENA_MARGIN) << FIXED_SHIFT))
    game->player.y = (236 - ARENA_MARGIN) << FIXED_SHIFT;
  if (game->player.invuln_timer > 0) game->player.invuln_timer--;
  /* Vital Heart regeneration. */
  {
    int vital = game->player.passive_level[PASSIVE_VITAL];
    if (vital > 0 && game->player.hp < game->player.max_hp) {
      game->player.regen_timer--;
      if (game->player.regen_timer <= 0) {
        game->player.hp += vital;
        if (game->player.hp > game->player.max_hp)
          game->player.hp = game->player.max_hp;
        game->player.regen_timer = 90;
      }
    }
  }
}

/* Seek the player with shift-only steering and no sqrt. Bats zigzag. */
static void update_enemies(struct Game* game) {
  struct Enemy* e = game->enemies;
  int px = game->player.x;
  int py = game->player.y;
  int tickband = (game->tick >> 4) & 1;
  int i = 0;
  while (i < 48) {
    if (e->active) {
      int dx = px - e->x;
      int dy = py - e->y;
      int adx = abs_int(dx);
      int ady = abs_int(dy);
      int speed = e->speed;
      int vx = 0;
      int vy = 0;
      if (e->slow_timer > 0) {
        e->slow_timer--;
        speed = speed >> 1;
      }
      if (e->hit_timer > 0) e->hit_timer--;
      if (adx + ady != 0) {
        if (adx > ady) {
          int mag = seek_minor(speed, ady, adx);
          vx = dx > 0 ? speed : -speed;
          if (dy > 0) vy = mag;
          else if (dy < 0) vy = -mag;
        } else {
          int mag = seek_minor(speed, adx, ady);
          vy = dy > 0 ? speed : -speed;
          if (dx > 0) vx = mag;
          else if (dx < 0) vx = -mag;
        }
        /* Bats weave perpendicular, alternating on a tick band per bat. */
        if (e->kind == ENEMY_BAT) {
          int weave = (((tickband + i) & 1) == 0) ? 1 : -1;
          int quarter = speed >> 2;
          if (weave < 0) quarter = -quarter;
          if (adx > ady) vy += quarter;
          else vx += quarter;
        }
        e->x += vx;
        e->y += vy;
      }
    }
    e++;
    i++;
  }
  /* Gentle pairwise separation so the horde spreads instead of stacking. */
  {
    struct Enemy* a = game->enemies;
    int ai = 0;
    while (ai < 48) {
      if (a->active) {
        struct Enemy* b = a;
        int bi = ai + 1;
        b++;
        while (bi < 48) {
          if (b->active) {
            int dx = a->x - b->x;
            int dy = a->y - b->y;
            int adx = abs_int(dx) >> 8;
            int ady = abs_int(dy) >> 8;
            if (adx < 8 && ady < 8) {
              int push = 32;
              if (adx + ady == 0) {
                /* Exactly stacked: split along x. */
                a->x += push;
                b->x -= push;
              } else if (adx > ady) {
                if (dx > 0) {
                  a->x += push;
                  b->x -= push;
                } else {
                  a->x -= push;
                  b->x += push;
                }
              } else {
                if (dy > 0) {
                  a->y += push;
                  b->y -= push;
                } else {
                  a->y -= push;
                  b->y += push;
                }
              }
            }
          }
          b++;
          bi++;
        }
      }
      a++;
      ai++;
    }
  }
}

/* Record a kill: streak bonus for chains within one second. */
static void register_kill(struct Game* game, int kind) {
  if (game->tick - game->last_kill_tick <= 30) game->streak++;
  else game->streak = 1;
  game->last_kill_tick = game->tick;
  game->kills++;
  game->score += enemy_score(kind) + ((game->streak - 1) << 2) +
    (game->streak - 1);
}

/* Damage one enemy, returning true when the blow killed it. */
static int damage_enemy(struct Game* game, struct Enemy* e, int dmg) {
  int kind = 0;
  int ex = 0;
  int ey = 0;
  e->hp -= dmg;
  e->hit_timer = 4;
  if (e->hp > 0) return 0;
  e->active = 0;
  kind = e->kind;
  ex = e->x;
  ey = e->y;
  register_kill(game, kind);
  drop_gem(game, ex, ey, enemy_xp(kind));
  if (kind == ENEMY_BOSS) {
    int g = 0;
    burst(game, ex, ey, 24, 512, 20, 0xF0F);
    game->shake_timer = 20;
    while (g < 4) {
      drop_gem(game, ex, ey, 5);
      g++;
    }
  } else {
    burst(game, ex, ey, 6, 384, 12, 0xF44);
  }
  game->sound |= SOUND_HIT;
  return 1;
}

/* Splash damage around a point for fireball impacts. */
static void splash(struct Game* game, int x, int y, int radius, int dmg) {
  int r2 = radius * radius;
  struct Enemy* e = game->enemies;
  int i = 0;
  while (i < 48) {
    if (e->active) {
      int d2 = dist2_fixed(x, y, e->x, e->y);
      if (d2 <= r2) damage_enemy(game, e, dmg);
    }
    e++;
    i++;
  }
  burst(game, x, y, 12, 448, 14, 0xF80);
  game->sound |= SOUND_EXPLODE;
  if (game->shake_timer < 6) game->shake_timer = 6;
}

/* Move projectiles and collide them with enemies. */
static void update_projectiles(struct Game* game) {
  struct Projectile* pr = game->projectiles;
  int p = 0;
  while (p < 64) {
    if (pr->active) {
      int px = 0;
      int py = 0;
      struct Enemy* e = game->enemies;
      int i = 0;
      int kind = 0;
      int dmg = 0;
      pr->x += pr->vx;
      pr->y += pr->vy;
      pr->life--;
      px = pr->x >> FIXED_SHIFT;
      py = pr->y >> FIXED_SHIFT;
      kind = pr->kind;
      dmg = pr->damage;
      if (pr->life <= 0 || px < 0 || px >= 320 || py < 0 || py >= 236) {
        pr->active = 0;
      } else {
        while (i < 48) {
          if (e->active) {
            int ex = e->x >> FIXED_SHIFT;
            int ey = e->y >> FIXED_SHIFT;
            int dx = abs_int(px - ex);
            int dy = abs_int(py - ey);
            int rad = enemy_radius(e->kind) + 2;
            if (dx <= rad && dy <= rad) {
              if (kind == SPELL_FIREBALL) {
                int fdx = pr->x;
                int fdy = pr->y;
                pr->active = 0;
                splash(game, fdx, fdy, 20, dmg);
                break;
              }
              damage_enemy(game, e, dmg);
              if (kind == SPELL_FROST) {
                e->slow_timer = 60;
              }
              if (pr->pierce > 0) {
                pr->pierce--;
              } else {
                pr->active = 0;
                break;
              }
            }
          }
          e++;
          i++;
        }
      }
    }
    pr++;
    p++;
  }
  /* Reap arc/nova kills: those spells damage directly without projectiles. */
  {
    struct Enemy* e = game->enemies;
    int k = 0;
    while (k < 48) {
      if (e->active && e->hp <= 0) {
        int kind = e->kind;
        int ex = e->x;
        int ey = e->y;
        e->active = 0;
        register_kill(game, kind);
        drop_gem(game, ex, ey, enemy_xp(kind));
        burst(game, ex, ey, 6, 384, 12, 0xFF0);
      }
      e++;
      k++;
    }
  }
}

/* Apply one tick of friction without multiply or divide. */
static int friction_step(int v) {
  v -= v >> 4;
  if (v > 0) v--;
  else if (v < 0) v++;
  return v;
}

/* Scatter gems with friction, pull them with the magnet, collect close ones.
 * A cheap box test skips the squared-distance multiplies for far gems. */
static void update_gems(struct Game* game) {
  int magnet = magnet_radius(&game->player);
  int magnet2 = magnet * magnet;
  int px = game->player.x;
  int py = game->player.y;
  int ppx = px >> FIXED_SHIFT;
  int ppy = py >> FIXED_SHIFT;
  struct Gem* g = game->gems;
  int i = 0;
  while (i < 64) {
    if (g->active) {
      int gx = g->x >> FIXED_SHIFT;
      int gy = g->y >> FIXED_SHIFT;
      int mdx = abs_int(gx - ppx);
      int mdy = abs_int(gy - ppy);
      int d2 = 0;
      if (mdx > magnet || mdy > magnet) {
        g->x += g->vx;
        g->y += g->vy;
        g->vx = friction_step(g->vx);
        g->vy = friction_step(g->vy);
        g++;
        i++;
        continue;
      }
      d2 = dist2_fixed(g->x, g->y, px, py);
      if (d2 <= 64) {
        int gain = g->value * xp_percent(&game->player) / 100;
        if (gain < 1) gain = 1;
        g->active = 0;
        game->player.xp += gain;
        game->sound |= SOUND_PICKUP;
      } else if (d2 <= magnet2) {
        int dx = px - g->x;
        int dy = py - g->y;
        int adx = abs_int(dx);
        int ady = abs_int(dy);
        int speed = 768;
        if (adx > ady) {
          int mag = seek_minor(speed, ady, adx);
          g->x += dx > 0 ? speed : -speed;
          if (dy > 0) g->y += mag;
          else if (dy < 0) g->y -= mag;
        } else if (adx + ady != 0) {
          int mag = seek_minor(speed, adx, ady);
          g->y += dy > 0 ? speed : -speed;
          if (dx > 0) g->x += mag;
          else if (dx < 0) g->x -= mag;
        }
      } else {
        g->x += g->vx;
        g->y += g->vy;
        g->vx = friction_step(g->vx);
        g->vy = friction_step(g->vy);
      }
    }
    g++;
    i++;
  }
}

/* Drift particles and retire expired ones. */
static void update_particles(struct Game* game) {
  struct Particle* p = game->particles;
  int i = 0;
  while (i < 96) {
    if (p->active) {
      p->x += p->vx;
      p->y += p->vy;
      p->vx = friction_step(p->vx);
      p->vy = friction_step(p->vy);
      p->life--;
      if (p->life <= 0) p->active = 0;
    }
    p++;
    i++;
  }
  if (game->shake_timer > 0) game->shake_timer--;
}

/* Hurt the warden on contact when vulnerability allows it. */
static void check_player_hits(struct Game* game) {
  struct Enemy* e = game->enemies;
  int px = game->player.x >> FIXED_SHIFT;
  int py = game->player.y >> FIXED_SHIFT;
  int i = 0;
  if (game->player.invuln_timer > 0) return;
  while (i < 48) {
    if (e->active) {
      int ex = e->x >> FIXED_SHIFT;
      int ey = e->y >> FIXED_SHIFT;
      int dx = abs_int(px - ex);
      int dy = abs_int(py - ey);
      int rad = enemy_radius(e->kind) + 4;
      if (dx <= rad && dy <= rad) {
        game->player.hp -= e->damage;
        game->player.invuln_timer = INVULN_TICKS;
        game->sound |= SOUND_HURT;
        game->shake_timer = 12;
        burst(game, game->player.x, game->player.y, 10, 448, 14, 0xF00);
        break;
      }
    }
    e++;
    i++;
  }
}

/* Whether an upgrade kind can still be offered. */
static int upgrade_available(struct Game* game, int kind) {
  if (kind < 5) {
    return game->player.spell_level[kind] < 5;
  }
  if (kind < 10) {
    return game->player.passive_level[kind - 5] < 5;
  }
  return 1;
}

/* Resulting level for a card, or 0 for repeatable bonuses. */
static int upgrade_result_level(struct Game* game, int kind) {
  if (kind < 5) return game->player.spell_level[kind] + 1;
  if (kind < 10) return game->player.passive_level[kind - 5] + 1;
  return 0;
}

/* Offer three distinct available upgrades. */
static void generate_choices(struct Game* game) {
  int n = 0;
  int guard = 0;
  while (n < 3 && guard < 200) {
    int kind = (int)(next_rand(game) % 12);
    int dup = 0;
    int k = 0;
    guard++;
    if (!upgrade_available(game, kind)) continue;
    while (k < n) {
      if (game->choices[k].kind == kind) dup = 1;
      k++;
    }
    if (dup) continue;
    game->choices[n].kind = kind;
    game->choices[n].level = upgrade_result_level(game, kind);
    n++;
  }
  /* Bonuses are always available, so the guard never exhausts them. */
  while (n < 3) {
    game->choices[n].kind = UPGRADE_BONUS_HEAL;
    game->choices[n].level = 0;
    n++;
  }
}

/* Apply a level-up card. */
static void apply_upgrade(struct Game* game, int kind) {
  if (kind < 5) {
    game->player.spell_level[kind]++;
    if (game->player.cooldown[kind] > 10) game->player.cooldown[kind] = 10;
  } else if (kind < 10) {
    int p = kind - 5;
    game->player.passive_level[p]++;
    if (p == PASSIVE_VITAL) {
      game->player.max_hp += 20;
      game->player.hp += 20;
    }
  } else if (kind == UPGRADE_BONUS_HEAL) {
    game->player.hp += 30;
    if (game->player.hp > game->player.max_hp)
      game->player.hp = game->player.max_hp;
  } else {
    game->score += 250;
  }
}

/* Reset run state for a fresh attempt, keeping best and rng. */
static void start_run(struct Game* game) {
  int i = 0;
  game->phase = PHASE_PLAY;
  game->tick = 0;
  game->player.x = 160 * FIXED_ONE;
  game->player.y = 118 * FIXED_ONE;
  game->player.hp = START_HP;
  game->player.max_hp = START_HP;
  game->player.level = 1;
  game->player.xp = 0;
  game->player.xp_next = xp_threshold(1);
  game->player.invuln_timer = 0;
  game->player.regen_timer = 90;
  game->player.face_x = 0;
  game->player.face_y = 1;
  while (i < 5) {
    game->player.spell_level[i] = 0;
    game->player.passive_level[i] = 0;
    game->player.cooldown[i] = 10;
    i++;
  }
  game->player.spell_level[SPELL_EMBER] = 1;
  i = 0;
  while (i < 48) {
    game->enemies[i].active = 0;
    i++;
  }
  i = 0;
  while (i < 64) {
    game->projectiles[i].active = 0;
    game->gems[i].active = 0;
    i++;
  }
  i = 0;
  while (i < 96) {
    game->particles[i].active = 0;
    i++;
  }
  game->spawn_timer = 20;
  game->boss_timer = BOSS_EVERY_TICKS;
  game->boss_count = 0;
  game->kills = 0;
  game->score = 0;
  game->shake_timer = 0;
  game->streak = 0;
  game->last_kill_tick = -1000;
}

/* Start on the title screen with a seed and saved best. */
void game_init(struct Game* game, unsigned seed, unsigned best) {
  game->phase = PHASE_TITLE;
  game->return_phase = PHASE_TITLE;
  game->tick = 0;
  game->rng = seed ? seed : 0xEBFA11u;
  game->best = (int)best;
  game->sound = 0;
  game->kills = 0;
  game->score = 0;
  start_run(game);
  game->phase = PHASE_TITLE;
}

/* Whole seconds survived at 30 ticks per second. */
int game_seconds(struct Game* game) {
  return game->tick / 30;
}

/* Short card title for an upgrade kind. */
char* upgrade_title(int kind) {
  if (kind == UPGRADE_SPELL_EMBER) return "Ember Bolt";
  if (kind == UPGRADE_SPELL_FIREBALL) return "Fireball";
  if (kind == UPGRADE_SPELL_FROST) return "Frost Shard";
  if (kind == UPGRADE_SPELL_ARC) return "Lightning Arc";
  if (kind == UPGRADE_SPELL_NOVA) return "Holy Nova";
  if (kind == UPGRADE_PASSIVE_SWIFT) return "Swift Boots";
  if (kind == UPGRADE_PASSIVE_POWER) return "Power Rune";
  if (kind == UPGRADE_PASSIVE_VITAL) return "Vital Heart";
  if (kind == UPGRADE_PASSIVE_MAGNET) return "Magnet Charm";
  if (kind == UPGRADE_PASSIVE_TOME) return "XP Tome";
  if (kind == UPGRADE_BONUS_HEAL) return "Warm Meal";
  return "Coin Purse";
}

/* One-line card text for a kind at a resulting level. */
char* upgrade_desc(int kind, int level) {
  if (kind == UPGRADE_SPELL_EMBER) {
    if (level <= 1) return "Auto bolt at nearest foe";
    if (level == 3) return "+1 bolt, faster, stronger";
    if (level == 5) return "+1 bolt, faster, stronger";
    return "Faster, stronger bolts";
  }
  if (kind == UPGRADE_SPELL_FIREBALL) {
    if (level <= 1) return "Slow orb with big splash";
    if (level == 4) return "+1 orb, bigger splash";
    return "Bigger, faster fireballs";
  }
  if (kind == UPGRADE_SPELL_FROST) {
    if (level <= 1) return "Shards that slow foes";
    if (level == 3 || level == 5) return "+1 shard, longer slow";
    return "Stronger, faster shards";
  }
  if (kind == UPGRADE_SPELL_ARC) {
    if (level <= 1) return "Chain zap, 3 targets";
    if (level == 3 || level == 5) return "+1 chain, harder zap";
    return "Harder, faster arcs";
  }
  if (kind == UPGRADE_SPELL_NOVA) {
    if (level <= 1) return "Ring blast around you";
    return "Wider, harder, faster ring";
  }
  if (kind == UPGRADE_PASSIVE_SWIFT) return "+8% move speed";
  if (kind == UPGRADE_PASSIVE_POWER) return "+12% all damage";
  if (kind == UPGRADE_PASSIVE_VITAL) return "+20 max HP, regen";
  if (kind == UPGRADE_PASSIVE_MAGNET) return "+25% pickup range";
  if (kind == UPGRADE_PASSIVE_TOME) return "+12% XP gain";
  if (kind == UPGRADE_BONUS_HEAL) return "Restore 30 HP now";
  return "+250 score now";
}

/* Advance the full run state machine by one 30 Hz tick. */
void game_tick(struct Game* game, struct GameInput input) {
  game->sound = 0;
  if (game->phase == PHASE_TITLE) {
    update_particles(game);
    if (input.start || input.confirm) {
      start_run(game);
      game->sound |= SOUND_UI;
    } else if (input.pause) {
      game->return_phase = PHASE_TITLE;
      game->phase = PHASE_HELP;
      game->sound |= SOUND_UI;
    }
    return;
  }
  if (game->phase == PHASE_HELP) {
    if (input.start || input.confirm || input.pause) {
      game->phase = game->return_phase;
      game->sound |= SOUND_UI;
    }
    return;
  }
  if (game->phase == PHASE_PAUSE) {
    if (input.pause || input.confirm) {
      game->phase = PHASE_PLAY;
      game->sound |= SOUND_UI;
    } else if (input.start) {
      start_run(game);
      game->sound |= SOUND_UI;
    }
    return;
  }
  if (game->phase == PHASE_GAMEOVER) {
    update_particles(game);
    if (input.start || input.confirm) {
      start_run(game);
      game->sound |= SOUND_UI;
    }
    return;
  }
  if (game->phase == PHASE_LEVELUP) {
    if (input.choose >= 0 && input.choose < 3) {
      int kind = game->choices[input.choose].kind;
      apply_upgrade(game, kind);
      game->player.xp -= game->player.xp_next;
      game->player.level++;
      game->player.xp_next = xp_threshold(game->player.level);
      game->sound |= SOUND_UI;
      if (game->player.xp >= game->player.xp_next) {
        generate_choices(game);
      } else {
        game->phase = PHASE_PLAY;
      }
    }
    return;
  }
  /* PHASE_PLAY: full simulation. */
  game->tick++;
  if (game->tick % 30 == 0) game->score += 5;
  if (input.pause) {
    game->phase = PHASE_PAUSE;
    game->sound |= SOUND_UI;
    return;
  }
  update_player(game, input);
  /* Spawning quickens and thickens with survived time. */
  {
    int sec = game_seconds(game);
    game->spawn_timer--;
    if (game->spawn_timer <= 0) {
      int batch = spawn_batch(sec);
      int b = 0;
      while (b < batch) {
        spawn_enemy(game, pick_spawn_kind(game, sec));
        b++;
      }
      game->spawn_timer = spawn_interval(sec);
    }
    game->boss_timer--;
    if (game->boss_timer <= 0) {
      spawn_boss(game);
      game->boss_count++;
      game->boss_timer = BOSS_EVERY_TICKS;
    }
  }
  update_spells(game);
  update_enemies(game);
  update_projectiles(game);
  update_gems(game);
  update_particles(game);
  check_player_hits(game);
  if (game->player.hp <= 0) {
    game->player.hp = 0;
    game->phase = PHASE_GAMEOVER;
    game->sound |= SOUND_GAMEOVER;
    game->shake_timer = 30;
    burst(game, game->player.x, game->player.y, 30, 640, 30, 0xF00);
    burst(game, game->player.x, game->player.y, 20, 448, 24, 0xFF0);
    if (game->score > game->best) game->best = game->score;
    return;
  }
  if (game->score > game->best) game->best = game->score;
  if (game->player.xp >= game->player.xp_next) {
    generate_choices(game);
    game->phase = PHASE_LEVELUP;
    game->sound |= SOUND_LEVELUP;
    burst(game, game->player.x, game->player.y, 20, 512, 20, 0x0F0);
  }
}
