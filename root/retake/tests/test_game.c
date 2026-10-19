/*
 * Regression tests for shipping game rules with generated Stage 1 rooms.
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

/* Run adversarial contacts before broader room solution replays. */
int main(void) {
  if (sim_open("../retake.pak")) return 1;
  test_up_exit();
  test_blocked_exit();
  test_gate_obstruction();
  test_boss_rewards();
  test_hazard_door();
  test_spring_impulse();
  test_room_ticks();
  test_snapshot_replay();
  test_edit_snapshot();
  test_rewind_support();
  test_crumble_timing();
  sim_close();
  if (!failures) puts("***PASS retake game rule regressions");
  return failures != 0;
}
