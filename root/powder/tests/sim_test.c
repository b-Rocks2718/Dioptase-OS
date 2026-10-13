/*
 * Deterministic host checks for the Powderworks cellular rules.
 * Exercises falling, flowing, floating, burning, quenching, dissolving,
 * melting, cooling, and growing without any display or device dependency.
 */

#include "../sim.h"

#include <assert.h>
#include <stdio.h>

/* Run the world forward for a fixed number of steps. */
static void run_steps(int steps) {
  for (int i = 0; i < steps; i++) sim_step();
  sim_recount();
}

/* Sand poured above the tray must land on the floor and be conserved. */
static void test_sand_falls_and_piles(void) {
  sim_init(12345);
  sim_paint(80, 20, 4, EL_SAND);
  int before = sim_counts[EL_SAND];
  assert(before > 20);
  run_steps(200);
  assert(sim_counts[EL_SAND] == before);
  assert(sim_at(80, SIM_H - 3) == EL_SAND || sim_at(80, SIM_H - 4) == EL_SAND);
  assert(sim_at(80, 20) == EL_EMPTY);
}

/* Water poured into a stone basin must stay inside and settle low. */
static void test_water_settles_in_basin(void) {
  sim_init(777);
  for (int x = 60; x <= 100; x++) sim_paint(x, 100, 0, EL_STONE);
  sim_paint(80, 40, 5, EL_WATER);
  int before = sim_counts[EL_WATER];
  assert(before > 20);
  run_steps(300);
  assert(sim_counts[EL_WATER] == before);
  for (int i = 0; i < SIM_CELLS; i++) {
    if (sim_cells[i] == EL_WATER) assert(i / SIM_W >= 90);
  }
}

/* Oil poured onto water must end up floating above it. */
static void test_oil_floats_on_water(void) {
  sim_init(4242);
  sim_paint(80, 100, 6, EL_WATER);
  run_steps(200);
  sim_paint(80, 95, 4, EL_OIL);
  run_steps(300);
  int oil_top = SIM_H;
  int water_top = SIM_H;
  for (int y = 0; y < SIM_H; y++) {
    if (sim_at(80, y) == EL_OIL && y < oil_top) oil_top = y;
    if (sim_at(80, y) == EL_WATER && y < water_top) water_top = y;
  }
  assert(oil_top < SIM_H && water_top < SIM_H);
  assert(oil_top <= water_top);
}

/* A fire lit in a wooden block must consume wood and then die out. */
static void test_fire_burns_wood_and_dies(void) {
  sim_init(99);
  sim_paint(80, 100, 6, EL_WOOD);
  int wood_before = sim_counts[EL_WOOD];
  sim_paint(80, 100, 1, EL_FIRE);
  run_steps(40);
  assert(sim_counts[EL_WOOD] < wood_before || sim_counts[EL_FIRE] > 0);
  run_steps(600);
  assert(sim_counts[EL_FIRE] == 0);
}

/* Water dropped into lava must quench some lava into stone. */
static void test_lava_quenches_to_stone(void) {
  sim_init(31337);
  sim_paint(80, 105, 5, EL_LAVA);
  run_steps(50);
  int stone_before = sim_counts[EL_STONE];
  sim_paint(80, 95, 4, EL_WATER);
  run_steps(200);
  assert(sim_counts[EL_STONE] > stone_before);
}

/* Acid resting on sand must dissolve some of the pile. */
static void test_acid_dissolves_sand(void) {
  sim_init(2024);
  sim_paint(80, 105, 6, EL_SAND);
  run_steps(100);
  int sand_before = sim_counts[EL_SAND];
  sim_paint(80, 95, 3, EL_ACID);
  run_steps(400);
  assert(sim_counts[EL_SAND] < sand_before);
}

/* A lone ember must cool into stone ash instead of burning forever. */
static void test_ember_cools_to_ash(void) {
  sim_init(555);
  sim_paint(40, 105, 1, EL_EMBER);
  run_steps(400);
  assert(sim_counts[EL_EMBER] == 0);
  assert(sim_counts[EL_STONE] > 0);
}

/* Snow banked against lava must melt into water. */
static void test_snow_melts_near_lava(void) {
  sim_init(8080);
  sim_paint(70, 105, 4, EL_LAVA);
  run_steps(20);
  sim_paint(76, 105, 2, EL_SNOW);
  run_steps(120);
  assert(sim_counts[EL_WATER] > 0);
}

/* A vine beside water must grow beyond its seed cell over time. */
static void test_plant_grows_with_water(void) {
  sim_init(60606);
  sim_paint(80, 105, 5, EL_WATER);
  run_steps(100);
  /* Seed the vine on the ground beside the settled puddle. */
  int seeded = 0;
  for (int x = PLAY_X0; x <= PLAY_X1 && !seeded; x++) {
    if (sim_at(x, SIM_H - 3) == EL_EMPTY &&
        (sim_at(x + 1, SIM_H - 3) == EL_WATER ||
         sim_at(x - 1, SIM_H - 3) == EL_WATER)) {
      sim_paint(x, SIM_H - 3, 0, EL_PLANT);
      seeded = 1;
    }
  }
  assert(seeded);
  run_steps(3000);
  assert(sim_counts[EL_PLANT] > 1);
}

/* The demo scene must contain every headline element after settling. */
static void test_demo_scene_contents(void) {
  sim_init(1);
  sim_demo_scene();
  run_steps(120);
  assert(sim_counts[EL_WOOD] > 0);
  assert(sim_counts[EL_OIL] > 0);
  assert(sim_counts[EL_WATER] > 0);
  assert(sim_counts[EL_LAVA] > 0);
  assert(sim_counts[EL_SAND] > 0);
}

int main(void) {
  test_sand_falls_and_piles();
  test_water_settles_in_basin();
  test_oil_floats_on_water();
  test_fire_burns_wood_and_dies();
  test_lava_quenches_to_stone();
  test_acid_dissolves_sand();
  test_ember_cools_to_ash();
  test_snow_melts_near_lava();
  test_plant_grows_with_water();
  test_demo_scene_contents();
  printf("powder sim tests passed\n");
  return 0;
}
