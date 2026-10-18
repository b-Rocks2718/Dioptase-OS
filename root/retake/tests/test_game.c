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
  sim_close();
  if (!failures) puts("***PASS retake game rule regressions");
  return failures != 0;
}
