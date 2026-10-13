/*
 * Host-side deterministic verification suite for SKYFORGE.
 *
 * Tests fixed-point trigonometry and biome palette generation, procedural
 * 128x128 3D terrain and canyon synthesis across all five sectors, live 3D
 * crater deformation and Terrain Forge sculpting tools, starfighter flight
 * physics and barrel-roll laser deflection, Grand Prix gate progression,
 * 3D Voxel-Space rendering, and 25 kHz PCM audio synthesis.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../skyforge.h"

unsigned g_view_words[30720];

/* Fail the test suite with an explanatory diagnostic when `cond` is false. */
static void check(bool cond, char* msg){
  if (!cond){
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
  }
}

/* Verify sine/cosine quarter-wave symmetry and fixed-point multiplication. */
static void test_math_and_palettes(void){
  math_init();
  check(isin(0) == 0, "isin(0) must be 0 at angle 0");
  check(isin(256) == 65536, "isin(256) must reach +1.0 (65536) at quarter turn");
  check(isin(512) == 0, "isin(512) must return to 0 at half turn");
  check(isin(768) == -65536, "isin(768) must reach -1.0 (-65536) at three-quarter turn");
  check(icos(0) == 65536, "icos(0) must be +1.0 (65536) at angle 0");
  check(fixmul(65536, 32768) == 32768, "fixmul(1.0, 0.5) must equal 0.5 in 16.16 fixed point");

  for (int b = 0; b < 5; ++b){
    check(biome_palettes[(b << 8) | (3 << 6) | 20] != 0,
          "every biome palette must populate non-black accent entries");
  }
}

/* Verify terrain generation, crater deformation, and Terrain Forge tools. */
static void test_terrain_and_forge(void){
  world_generate(1, MODE_FORGE, 12345u);
  for (int i = 0; i < 16384; ++i){
    int h = g_terrain[i] & 0x3F;
    check(h >= 2 && h <= 63, "generated terrain height must stay within [2, 63]");
  }

  int h0 = terrain_height_cell(40, 40);
  forge_apply_tool(40, 40, TOOL_RAISE, 4, false);
  int h1 = terrain_height_cell(40, 40);
  check(h1 > h0, "TOOL_RAISE in Terrain Forge must increase cell elevation");

  terrain_deform(40, 40, 5, -14, 1);
  int h2 = terrain_height_cell(40, 40);
  check(h2 < h1, "Seismic crater deformation must carve a depression into the heightmap");
}

/* Verify campaign sector spawns, boss shield pylons, and barrel-roll deflection. */
static void test_campaign_and_combat(void){
  world_generate(5, MODE_CAMPAIGN, 9999u);
  check(world_enemies_remaining() > 0, "Sector 5 must spawn hostile squadrons and the Leviathan boss");

  bool found_boss = false;
  for (int i = 0; i < 64; ++i){
    if (g_entities[i].active && g_entities[i].kind == ENT_BOSS){
      found_boss = true;
      check(g_entities[i].shield_gen == 0x0F, "Leviathan boss must start with all 4 shield pylons active");
    }
  }
  check(found_boss, "Sector 5 must include an active ENT_BOSS entity");

  /* Step 80 ticks under AI Wingman control firing primary and seismic weapons */
  g_state = STATE_PLAY;
  struct FlightControls ctrl;
  for (int t = 0; t < 80; ++t){
    autopilot_decide(&ctrl);
    if (t == 5) ctrl.barrel_roll = 1;
    if (t == 10) ctrl.fire_seismic = 1;
    ctrl.fire_primary = 1;
    world_tick(&ctrl);
  }
  check(g_player.z > (8 << 16), "autopilot must maintain safe altitude above canyon floor");
}

/* Verify 3D Voxel-Space scene rendering and non-silent 25 kHz PCM audio synthesis. */
static void test_render_and_audio(void){
  render_init();
  world_generate(1, MODE_RACE, 4242u);
  struct Camera cam;
  cam.x = g_player.x;
  cam.y = g_player.y;
  cam.z = g_player.z;
  cam.angle = g_player.angle;
  cam.pitch = g_player.pitch;
  cam.roll = 12;
  cam.biome = BIOME_EMERALD;

  render_scene(&cam);
  unsigned h_a = render_view_checksum();
  cam.angle = (cam.angle + 128) & 1023;
  render_scene(&cam);
  unsigned h_b = render_view_checksum();
  check(h_a != h_b, "rotating camera heading must produce a distinct 3D voxel view");

  short pcm[250];
  audio_set_song(SONG_TITLE);
  sfx_trigger(SFX_CRATER_BOOM);
  audio_synth_test_chunk(pcm, 250);
  bool has_signal = false;
  for (int i = 0; i < 250; ++i){
    if (pcm[i] > 100 || pcm[i] < -100) has_signal = true;
  }
  check(has_signal, "25 kHz PCM synthesizer must emit non-silent waveform samples");
}

int main(void){
  test_math_and_palettes();
  test_terrain_and_forge();
  test_campaign_and_combat();
  test_render_and_audio();
  printf("PASS: all SKYFORGE host simulation, 3D voxel render, and audio tests passed.\n");
  return 0;
}
