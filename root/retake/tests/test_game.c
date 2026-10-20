/*
 * Regression tests for shipping game rules with the generated campaign rooms.
 * Fixtures set up specific contacts; all assertions advance the actual tick
 * or room lifecycle. These are rule regressions, not campaign solution proofs.
 */
#include "sim.c"

static int failures;

/* Report the gameplay contract lost by a failed assertion. */
static void expect(int ok, char* why) {
  if (!ok) { fprintf(stderr, "***FAIL retake game: %s\n", why); failures++; }
}

/* Find a live fixture entity without depending on its packed list order. */
static struct Ent* entity(int kind) {
  for (int i = 0; i < g_ent_count; i++)
    if (g_ents[i].alive && g_ents[i].kind == kind) return &g_ents[i];
  return 0;
}

/* Jumping into an open ceiling must reach the room above after collision. */
static void test_up_exit(void) {
  game_new(ROOM_S1_CELLAR);
  player_init(&g_hero, 244, 1, 0);
  g_hero.vy = -1000;
  game_tick(IN_JUMP, 0);
  expect(g_room_index == ROOM_S1_ENTRY, "the cellar ceiling opening transitions upward");
  expect(g_hero.vy < 0, "an upward transition preserves a rising impulse");
}

/* Touching a seam cannot teleport Ruby into the neighbor's solid scenery. */
static void test_blocked_exit(void) {
  game_new(ROOM_S1_ENTRY);
  for (int y = 26; y < 30; y++) g_room.col[g_room.row[y] + 39] = COL_AIR;
  player_init(&g_hero, 312, 217, 0);
  game_tick(IN_RIGHT, 0);
  expect(g_room_index == ROOM_S1_ENTRY,
         "a side exit refuses a destination whose body overlaps solid tiles");
}

/* A released switch cannot place solid geometry through Ruby's body. */
static void test_gate_obstruction(void) {
  game_new(ROOM_S1_MARK);
  struct Ent* gate = entity(ENT_GATE);
  expect(gate != 0, "Mark fixture contains its gate");
  if (!gate) return;
  g_switch[gate->a] = 1;
  update_gates();
  player_init(&g_hero, gate->tx * TILE, 196, 0);
  game_tick(0, 0);
  expect(gate->state == 1 && player_can_place(&g_room, g_hero.x, g_hero.y),
         "gate remains open while Ruby occupies its column");
  player_init(&g_hero, 264, 196, 0);
  game_tick(0, 0);
  expect(gate->state == 0, "gate closes after Ruby has cleared its column");
}

/* A slate is a momentary switch: any clap, Ruby's or a take's, holds its
 * switch for b ticks and then lets the gate close. This is what makes
 * timing puzzles possible where a plate would only need a parked take. */
static void test_slate_hold(void) {
  game_new(ROOM_S1_MARK);
  struct Ent* gate = entity(ENT_GATE);
  struct Ent* slate = ent_alloc();
  expect(gate && slate, "Mark fixture has its gate and room for a slate");
  if (!gate || !slate) return;
  ent_setup(slate, ENT_SLATE, 5, 25, gate->a, 30, 0, -1);
  player_init(&g_hero, 24, 196, 0);
  game_tick(IN_ATTACK, IN_ATTACK);
  expect(slate->timer == 30 && (g_events & GE_HIT) != 0, "Ruby's clap marks a slate for its hold time");
  game_tick(0, 0);
  expect(gate->state == 1, "a marked slate opens its gate on the next tick");
  int ticks = 0;
  while (slate->timer > 0 && ticks < 60) {
    game_tick(0, 0);
    ticks++;
  }
  game_tick(0, 0);
  expect(slate->timer == 0 && gate->state == 0, "the gate closes once the slate's hold runs out");

  game_tick(0, IN_RECORD);
  game_tick(IN_ATTACK, IN_ATTACK);
  for (int i = 0; i < 12; i++) game_tick(0, 0);
  game_tick(0, IN_RECORD);
  player_init(&g_hero, 264, 196, 0);
  slate->timer = 0;
  int marked = 0;
  for (int i = 0; i < 40 && !marked; i++) {
    game_tick(0, 0);
    marked = slate->timer > 0;
  }
  expect(marked, "a take replaying a clap marks the slate while Ruby is across the room");
}

/* A gate with a switch mask opens only while every listed switch is held;
 * its release switch opens it alone (the lever on the far side). */
static void test_gate_all_switches(void) {
  game_new(ROOM_S1_MARK);
  struct Ent* gate = entity(ENT_GATE);
  expect(gate != 0, "Mark fixture contains its gate");
  if (!gate) return;
  player_init(&g_hero, 24, 196, 0);
  gate->c = (1 << 2) | (1 << 3);
  gate->d = 5;
  for (int i = 0; i < MAX_SWITCHES; i++) g_switch[i] = 0;
  g_switch[gate->a] = 1;
  g_switch[2] = 1;
  update_gates();
  expect(gate->state == 0, "a gate wanting three switches stays shut while one is missing");
  g_switch[3] = 1;
  update_gates();
  expect(gate->state == 1, "the gate opens once all three switches are held together");
  for (int i = 0; i < MAX_SWITCHES; i++) g_switch[i] = 0;
  update_gates();
  expect(gate->state == 0, "the gate shuts again when its switches are released");
  g_switch[4] = 1;
  update_gates();
  expect(gate->state == 1, "the release switch opens the gate by itself");
}

/* The club's stage spot holds its show switch only while it is locked on a
 * take, never on Ruby, and having no rifle it never fires at either. */
static void test_spotlight_show(void) {
  game_new(ROOM_S7_CLUB);
  struct Ent* spot = entity(ENT_SEARCHLIGHT);
  expect(spot != 0, "club fixture contains the stage spot");
  if (!spot) return;
  int sw = ((spot->d >> 12) & 7) - 1;
  expect(sw >= 0 && ((spot->d >> 8) & 1), "the stage spot has a show switch and no rifle");
  spot->state = 1;
  spot->vy = 2;
  update_switches();
  expect(g_switch[sw] == 1, "a spot locked on a take holds its show switch");
  spot->vy = 1;
  update_switches();
  expect(g_switch[sw] == 0, "a spot locked on Ruby does not");
  player_init(&g_hero, 232, 132, 0);
  spot->vx = (232 + (PLAYER_W >> 1)) << 8;
  spot->state = 0;
  int locked = 0;
  for (int i = 0; i < 80; i++) {
    game_tick(0, 0);
    if (spot->state == 1 && spot->vy == 1) locked++;
  }
  expect(locked > SEARCH_LOCK, "the spot finds Ruby on the club floor and stays on her");
  expect(entity(ENT_BOLT) == 0, "a spot without a rifle never fires");
}

/* Defeat is persistent, so its uncollected ability and exit must be too. */
static void test_boss_rewards(void) {
  game_new(ROOM_S1_CLOCK_TOP);
  struct Ent* boss = entity(ENT_BOSS);
  expect(boss != 0, "clock-top fixture contains Gearhart");
  if (!boss) return;
  boss->state = B_IDLE;
  boss->timer = 100;
  boss->hp = 1;
  player_init(&g_hero, (boss->x / Q8) - 18, 196, 0);
  game_tick(IN_ATTACK, IN_ATTACK);
  expect((g_events & GE_BOSS_DOWN) != 0, "a final clapper hit defeats Gearhart");
  expect(entity(ENT_ABILITY) && entity(ENT_DOOR), "defeat creates the wire and return door");
  game_enter_room(ROOM_S1_CLOCK_TOP, 160, 180);
  expect(!entity(ENT_BOSS) && entity(ENT_ABILITY) && entity(ENT_DOOR),
         "re-entry restores uncollected boss reward and return door without reviving boss");
}

/* Fatal spikes resolve before a coincident interaction can change rooms. */
static void test_hazard_door(void) {
  game_new(ROOM_S1_ENTRY);
  player_init(&g_hero, 24, 196, 0);
  g_health = 1;
  g_room.col[g_room.row[24] + 3] = COL_SPIKE;
  game_tick(IN_UP, IN_UP);
  expect((g_events & GE_DEATH) != 0 && g_room_index == ROOM_S1_ENTRY,
         "pressing Up in lethal spikes cannot use a door to bypass death");
}

/* Springs provide their authored height without requiring an unrelated key. */
static void test_spring_impulse(void) {
  game_new(ROOM_S1_ROOFTOPS);
  player_init(&g_hero, 416, 87, 0);
  g_hero.vy = 512;
  for (int i = 0; i < 6 && !(g_events & GE_SPRING); i++) game_tick(0, 0);
  expect((g_events & GE_SPRING) != 0, "falling onto the rooftop spring launches Ruby");
  game_tick(0, 0);
  expect(g_hero.vy < -1000, "releasing Jump does not cancel a spring impulse");
}

/* Exercise negative world positions and delayed enemy spawns under UBSan. */
static void test_room_ticks(void) {
  for (int room = 0; room < pak_room_count(); room++) {
    game_new(room);
    for (int tick = 0; tick < 480; tick++) game_tick(0, 0);
  }
}

/* Solver backtracking must reproduce enemies, motion and input-visible events. */
static void test_snapshot_replay(void) {
  struct SimSnapshot before;
  struct SimSnapshot after;
  struct SimSnapshot replayed;
  game_new(ROOM_S1_STREET);
  for (int i = 0; i < 30; i++) game_tick(IN_RIGHT, 0);
  sim_snapshot(&before);
  for (int i = 0; i < 30; i++) game_tick(IN_RIGHT | IN_JUMP, i == 0 ? IN_JUMP : 0);
  sim_snapshot(&after);
  sim_restore(&before);
  for (int i = 0; i < 30; i++) game_tick(IN_RIGHT | IN_JUMP, i == 0 ? IN_JUMP : 0);
  sim_snapshot(&replayed);
  expect(memcmp(&after, &replayed, sizeof(after)) == 0,
         "restoring a search snapshot reproduces the entire simulation exactly");
}

/* Search backtracking must preserve delayed abilities and the moon-room rules. */
static void test_edit_snapshot(void) {
  struct SimSnapshot before;
  struct SimSnapshot after;
  struct SimSnapshot replayed;
  game_new(ROOM_S6_GATE);
  g_prog.abilities = AB_OVERCRANK | AB_REWIND;
  g_hero.abilities = g_prog.abilities;
  for (int i = 0; i < 140; i++) game_tick(i < 30 ? IN_RIGHT : 0, 0);
  game_tick(IN_OVERCRANK, IN_OVERCRANK);
  sim_snapshot(&before);
  for (int i = 0; i < 20; i++) game_tick(IN_RIGHT, i == 5 ? IN_REWIND : 0);
  sim_snapshot(&after);
  /* Entering another region changes private state outside the Player struct. */
  game_new(ROOM_S1_ENTRY);
  sim_restore(&before);
  for (int i = 0; i < 20; i++) game_tick(IN_RIGHT, i == 5 ? IN_REWIND : 0);
  sim_snapshot(&replayed);
  expect(memcmp(&after, &replayed, sizeof(after)) == 0,
         "snapshots restore moon gravity, Overcrank phase and the full Rewind ring");
}

/* Rewind changes position, so departure support cannot grant a destination jump. */
static void test_rewind_support(void) {
  game_new(ROOM_S1_ENTRY);
  g_prog.abilities = AB_REWIND;
  g_hero.abilities = g_prog.abilities;
  player_init(&g_hero, 96, 196, AB_REWIND);
  game_tick(0, 0);
  rewind_count = 1;
  rewind_head = 1;
  rewind_x[0] = 80 * Q8;
  rewind_y[0] = 100 * Q8;
  g_hero.wall_lock = 8;
  game_tick(IN_REWIND | IN_JUMP | IN_RIGHT, IN_REWIND | IN_JUMP);
  expect((g_events & GE_REWIND) && !(g_hero.events & (EV_JUMP | EV_WALLJUMP | EV_WIRE)),
         "Rewind into open air cannot spend the departure floor's coyote jump");
  expect(g_hero.vy >= 0 && g_hero.vx > 0 && g_hero.wall_lock == 0,
         "Rewind clears old wall lock while retaining ordinary destination input");
}

/* Crumble warnings and absence use world ticks, and collision changes atomically. */
static void test_crumble_timing(void) {
  game_new(ROOM_S5_LIBRARY);
  struct Ent* block = entity(ENT_CRUMBLE);
  expect(block != 0, "library fixture supplies a crumble ledge");
  if (!block) return;
  int cell = g_room.row[block->ty] + block->tx;
  expect(g_room.col[cell] == COL_SOLID, "crumble collision exists before the room's first player step");
  player_init(&g_hero, block->tx * TILE, block->ty * TILE - PLAYER_H, 0);
  game_tick(0, 0);
  expect(block->state == 1 && block->timer == 30, "standing arms a full thirty-world-tick crumble warning");
  for (int i = 0; i < 10; i++) game_tick(0, 0);
  expect(block->state == 1 && block->timer == 20, "each normal tick advances a crumble timer exactly once");
  block->timer = 30;
  g_overcrank = 180;
  for (int i = 0; i < 10; i++) game_tick(0, 0);
  expect(block->state == 1 && block->timer == 25, "ten Overcrank ticks advance five crumble world ticks");
  g_overcrank = 0;
  block->timer = 1;
  game_tick(0, 0);
  expect(block->state == 2 && block->timer == 180 && g_room.col[cell] == COL_AIR,
         "collapse removes collision and begins the complete three-second absence");
  player_init(&g_hero, 32, 436, 0);
  for (int i = 0; i < 10; i++) game_tick(0, 0);
  expect(block->state == 2 && block->timer == 170, "gone time also advances exactly once per tick");
  player_init(&g_hero, block->tx * TILE, block->ty * TILE, 0);
  block->timer = 1;
  game_tick(0, 0);
  expect(block->state == 2 && g_room.col[cell] == COL_AIR,
         "a reforming block waits while Ruby overlaps it");
  player_init(&g_hero, 32, 436, 0);
  block->timer = 1;
  game_tick(0, 0);
  expect(block->state == 0 && g_room.col[cell] == COL_SOLID,
         "a clear block reforms collision immediately, without an unsafe air tick");
}

/* Active beat ledges must support an arrival before the first player sweep;
 * later phase changes may never fill the tile occupied by Ruby. */
static void test_beat_collision(void) {
  game_new(ROOM_S8_FLYLOFT);
  struct Ent* block = entity(ENT_BEAT);
  expect(block != 0, "flyloft fixture supplies an alternating beat ledge");
  if (!block) return;
  int tx = block->tx;
  int ty = block->ty;
  beat_parity = block->b & 1;
  beat_phase = 0;
  game_enter_room(ROOM_S8_FLYLOFT, tx * TILE, ty * TILE - PLAYER_H);
  block = entity(ENT_BEAT);
  int cell = g_room.row[ty] + tx;
  expect(block->state == 1 && g_room.col[cell] == COL_SOLID,
         "active beat collision exists immediately on room entry");
  int y = g_hero.y;
  game_tick(0, 0);
  expect(g_hero.on_ground && g_hero.y == y,
         "Ruby arriving on an active beat ledge cannot fall through its first tick");

  beat_parity ^= 1;
  update_beats();
  expect(!block->state && g_room.col[cell] == COL_AIR,
         "an inactive beat restores its underlying air collision");
  player_init(&g_hero, tx * TILE, ty * TILE, 0);
  beat_parity ^= 1;
  update_beats();
  expect(!block->state && g_room.col[cell] == COL_AIR,
         "a beat becoming active waits for Ruby to leave its interior");
  player_init(&g_hero, 24, 388, 0);
  update_beats();
  expect(block->state && g_room.col[cell] == COL_SOLID,
         "a deferred beat publishes collision as soon as its interior is clear");
  beat_phase = 0;
  g_overcrank = 180;
  for (int i = 0; i < 10; i++) game_tick(0, 0);
  expect(beat_phase == 5, "beat phase advances on five of ten Overcrank ticks");
}

/* Each late boss, and the Everlasting Camera, must fall to claps landed outside its guarded states. Ruby
 * is held beside it (facing it, health restored every tick, so its attacks
 * cannot end the run) and claps every 20 ticks; this proves the boss is
 * hittable and its defeat fires, not that a player will win. */
static void test_late_bosses_fall(void) {
  int rooms[6] = {ROOM_S4_ARENA, ROOM_S5_ARENA, ROOM_S6_THRONE, ROOM_S7_PENTHOUSE, ROOM_S8_FINALE, ROOM_TOWER_BOOTH};
  for (int i = 0; i < 6; i++) {
    game_new(rooms[i]);
    struct Ent* boss = entity(ENT_BOSS);
    expect(boss != 0, "each late boss arena contains its boss");
    if (!boss) continue;
    int fell = 0;
    for (int t = 0; t < 6000 && !fell; t++) {
      g_health = g_prog.max_health;
      if (boss->alive) {
        int bx = boss->x / Q8;
        int left = bx - 10 >= 16;
        g_hero.x = (left ? bx - 10 : bx + boss->w / Q8 + 2) * Q8;
        g_hero.y = boss->y + boss->h - PLAYER_H * Q8;
        g_hero.vx = 0;
        g_hero.vy = 0;
        g_hero.facing = left ? 1 : -1;
      }
      int clap = (t % 20) == 0 ? IN_ATTACK : 0;
      game_tick(clap, clap);
      if (g_events & GE_BOSS_DOWN) fell = 1;
    }
    expect(fell, "a late boss can be clapped down outside its guarded states");
  }
}

/* Run adversarial contacts before broader room solution replays. */
int main(void) {
  if (sim_open("../retake.pak")) return 1;
  test_up_exit();
  test_blocked_exit();
  test_gate_obstruction();
  test_slate_hold();
  test_gate_all_switches();
  test_spotlight_show();
  test_boss_rewards();
  test_hazard_door();
  test_spring_impulse();
  test_room_ticks();
  test_late_bosses_fall();
  test_snapshot_replay();
  test_edit_snapshot();
  test_rewind_support();
  test_crumble_timing();
  test_beat_collision();
  sim_close();
  if (!failures) puts("***PASS retake game rule regressions");
  return failures != 0;
}
