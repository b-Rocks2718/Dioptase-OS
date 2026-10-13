#include "skyforge.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"
#include "../crt/mouse.h"
#include "../crt/terminal.h"
#include "../crt/string.h"

/*
 * SKYFORGE main entry point, CLI mode dispatcher, responsive mouse-aim and
 * keyboard flight/hover-drone controls, 60 Hz frame loop, and guest self-test.
 * Signed by gemini-4-argon.
 */

volatile short* g_pixel_fb = NULL;
volatile short* g_tile_fb = NULL;
volatile short* g_tilemap = NULL;
volatile short* g_spritemap = NULL;
unsigned g_view_words[30720];

static bool raw_kb_active = false;
static int raw_pending_code = -1;
static unsigned char key_held[256];
static unsigned char key_pressed[256];

static int mouse_buttons = 0;
static int mouse_clicked = 0;
static int mouse_wheel_delta = 0;
static int mouse_idle_ticks = 0;
static int menu_cursor = 0;
static bool app_running = true;

/* Parse an unsigned decimal integer from string `s`. */
static unsigned parse_uint(char* s){
  unsigned v = 0;
  while (*s >= '0' && *s <= '9'){
    v = v * 10 + (unsigned)(*s - '0');
    s += 1;
  }
  return v;
}

/* Record a key press or release event into `key_held` and `key_pressed`. */
static void handle_key_event(int code, bool released, unsigned char hold_ticks){
  code &= 0xFF;
  if (code >= 'A' && code <= 'Z'){
    code = code - 'A' + 'a';
  }
  if (released){
    key_held[code] = 0;
  } else {
    if (key_held[code] == 0){
      key_pressed[code] = 1;
    }
    key_held[code] = hold_ticks;
  }
}

/* Poll keyboard (raw terminal pipe or fallback PS/2 getkey) and mouse stream. */
static void poll_inputs(void){
  for (int i = 0; i < 256; ++i){
    key_pressed[i] = 0;
    if (!raw_kb_active && key_held[i] > 0 && key_held[i] < 250){
      key_held[i] -= 1;
    }
  }
  mouse_clicked = 0;
  mouse_wheel_delta = 0;

  if (raw_kb_active){
    while (fd_bytes_available(STDIN) > 0){
      unsigned char b = 0;
      if (read(STDIN, &b, 1) != 1) break;
      if (raw_pending_code < 0){
        raw_pending_code = (int)b;
      } else {
        handle_key_event(raw_pending_code, b == TERMINAL_RAW_RELEASE, 255);
        raw_pending_code = -1;
      }
    }
  } else {
    while (fd_bytes_available(STDIN) > 0){
      unsigned char b = 0;
      if (read(STDIN, &b, 1) != 1) break;
      handle_key_event((int)b, false, 12);
    }
    while (true){
      unsigned ev = getkey();
      if (ev == 0) break;
      bool rel = (ev & 0x0100) != 0;
      handle_key_event((int)(ev & 0xFF), rel, 255);
    }
  }

  bool moved_mouse = false;
  while (true){
    int mev = getmouse();
    if (mev == 0) break;
    int btns = MOUSE_BUTTONS(mev);
    int dx = MOUSE_DX(mev);
    int dy = MOUSE_DY(mev);
    int wh = MOUSE_WHEEL(mev);

    if ((btns & MOUSE_BUTTON_LEFT) && !(mouse_buttons & MOUSE_BUTTON_LEFT)){
      mouse_clicked |= MOUSE_BUTTON_LEFT;
    }
    if ((btns & MOUSE_BUTTON_RIGHT) && !(mouse_buttons & MOUSE_BUTTON_RIGHT)){
      mouse_clicked |= MOUSE_BUTTON_RIGHT;
    }
    if ((btns & MOUSE_BUTTON_MIDDLE) && !(mouse_buttons & MOUSE_BUTTON_MIDDLE)){
      mouse_clicked |= MOUSE_BUTTON_MIDDLE;
    }
    mouse_buttons = btns;
    mouse_wheel_delta += wh;

    if (dx != 0 || dy != 0){
      moved_mouse = true;
      g_player.aim_x += dx;
      g_player.aim_y += dy;
      if (g_player.aim_x < -100) g_player.aim_x = -100;
      if (g_player.aim_x >  100) g_player.aim_x =  100;
      if (g_player.aim_y < -60)  g_player.aim_y = -60;
      if (g_player.aim_y >  60)  g_player.aim_y =  60;
    }
  }

  if (moved_mouse){
    mouse_idle_ticks = 0;
  } else {
    mouse_idle_ticks += 1;
    if (mouse_idle_ticks > 30 && g_mode != MODE_FORGE){
      if (g_player.aim_x > 0) g_player.aim_x -= 1;
      else if (g_player.aim_x < 0) g_player.aim_x += 1;
      if (g_player.aim_y > 0) g_player.aim_y -= 1;
      else if (g_player.aim_y < 0) g_player.aim_y += 1;
    }
  }
}

/* Switch into a chosen gameplay mode from the Home Screen menu. */
static void start_selected_mode(int item){
  g_demo_mode = false;
  if (item == 0){
    g_autopilot = false;
    g_state = STATE_PLAY;
    world_generate(1, MODE_CAMPAIGN, g_seed);
    audio_set_song(SONG_EMERALD);
  } else if (item == 1){
    g_autopilot = false;
    g_state = STATE_PLAY;
    world_generate(1, MODE_RACE, g_seed);
    audio_set_song(SONG_CYBER);
  } else if (item == 2){
    g_autopilot = false;
    g_state = STATE_PLAY;
    world_generate(1, MODE_FORGE, g_seed);
    audio_set_song(SONG_FORGE);
  } else if (item == 3){
    g_autopilot = true;
    g_state = STATE_PLAY;
    world_generate(1, MODE_CAMPAIGN, g_seed);
    audio_set_song(SONG_EMERALD);
    set_status_banner("AI WINGMAN ENGAGED - PRESS D FOR MANUAL", 150);
  } else if (item == 4){
    g_state = STATE_HELP;
  } else if (item == 5){
    app_running = false;
  }
}

/* Process state transitions and build one tick of player flight/drone controls. */
static void update_controls(struct FlightControls* ctrl){
  ctrl->turn = 0;
  ctrl->pitch = 0;
  ctrl->strafe = 0;
  ctrl->throttle = 0;
  ctrl->fire_primary = 0;
  ctrl->fire_seismic = 0;
  ctrl->barrel_roll = 0;
  ctrl->sculpt_apply = 0;
  ctrl->sculpt_erase = 0;

  if (key_pressed['m']){
    bool is_muted = audio_toggle_mute();
    set_status_banner(is_muted ? "AUDIO MUTED" : "AUDIO ENABLED", 60);
  }

  if (g_state == STATE_TITLE){
    if (key_pressed['w'] || key_pressed[KEY_UP]){
      menu_cursor = (menu_cursor + 5) % 6;
      sfx_trigger(SFX_RING_PASS);
    }
    if (key_pressed['s'] || key_pressed[KEY_DOWN]){
      menu_cursor = (menu_cursor + 1) % 6;
      sfx_trigger(SFX_RING_PASS);
    }
    if (key_pressed['1']) start_selected_mode(0);
    else if (key_pressed['2'] || key_pressed['r']) start_selected_mode(1);
    else if (key_pressed['3'] || key_pressed['t']) start_selected_mode(2);
    else if (key_pressed['4'] || key_pressed['d']) start_selected_mode(3);
    else if (key_pressed['5'] || key_pressed['h']) start_selected_mode(4);
    else if (key_pressed['q']) app_running = false;
    else if (key_pressed[KEY_ENTER] || key_pressed[' '] || (mouse_clicked & MOUSE_BUTTON_LEFT)){
      start_selected_mode(menu_cursor);
    }
    autopilot_decide(ctrl);
    return;
  }

  if (g_state == STATE_HELP){
    if (key_pressed[KEY_ENTER] || key_pressed[' '] || key_pressed['h'] ||
        key_pressed['q'] || key_pressed[0x1B] || (mouse_clicked & MOUSE_BUTTON_LEFT)){
      g_state = STATE_TITLE;
    }
    autopilot_decide(ctrl);
    return;
  }

  if (g_state == STATE_PAUSE){
    if (key_pressed['p'] || key_pressed[KEY_ENTER] || key_pressed[' ']){
      g_state = STATE_PLAY;
    } else if (key_pressed['t']){
      g_mode = (g_mode == MODE_FORGE) ? MODE_CAMPAIGN : MODE_FORGE;
      g_state = STATE_PLAY;
      audio_set_song(g_mode == MODE_FORGE ? SONG_FORGE : SONG_EMERALD);
    } else if (key_pressed['q']){
      g_state = STATE_TITLE;
      g_demo_mode = true;
      audio_set_song(SONG_TITLE);
    }
    return;
  }

  if (g_state == STATE_SECTOR_CLEAR){
    if (key_pressed[KEY_ENTER] || key_pressed[' '] || (mouse_clicked & MOUSE_BUTTON_LEFT)){
      int next_sec = g_sector + 1;
      if (next_sec > MAX_SECTORS) next_sec = 1;
      g_state = STATE_PLAY;
      world_generate(next_sec, MODE_CAMPAIGN, g_seed + (unsigned)next_sec);
      if (next_sec == MAX_SECTORS) audio_set_song(SONG_BOSS);
      else if (next_sec >= 3) audio_set_song(SONG_CYBER);
      else audio_set_song(SONG_EMERALD);
    } else if (key_pressed['q']){
      g_state = STATE_TITLE;
      g_demo_mode = true;
      audio_set_song(SONG_TITLE);
    }
    return;
  }

  if (g_state == STATE_VICTORY || g_state == STATE_GAMEOVER){
    if (key_pressed[KEY_ENTER] || key_pressed[' '] || key_pressed['r'] ||
        (mouse_clicked & MOUSE_BUTTON_LEFT)){
      g_state = STATE_PLAY;
      g_player.hp = 0;
      world_generate(1, g_mode, g_seed + g_frame);
      audio_set_song(g_mode == MODE_RACE ? SONG_CYBER : SONG_EMERALD);
    } else if (key_pressed['q']){
      g_state = STATE_TITLE;
      g_demo_mode = true;
      audio_set_song(SONG_TITLE);
    }
    return;
  }

  /* STATE_PLAY controls */
  if (key_pressed['p']){
    g_state = STATE_PAUSE;
    return;
  }
  if (key_pressed['q']){
    g_state = STATE_TITLE;
    g_demo_mode = true;
    audio_set_song(SONG_TITLE);
    return;
  }
  if (key_pressed['v']){
    g_player.cockpit_view = !g_player.cockpit_view;
    set_status_banner(g_player.cockpit_view ? "COCKPIT VIEW" : "CHASE ARWING VIEW", 75);
  }
  if (key_pressed['d']){
    g_autopilot = !g_autopilot;
    set_status_banner(g_autopilot ? "AI WINGMAN ENGAGED" : "MANUAL FLIGHT CONTROL", 75);
  }
  if (key_pressed['n']){
    int next_sec = (g_sector % MAX_SECTORS) + 1;
    world_generate(next_sec, g_mode, g_seed + (unsigned)next_sec);
    if (next_sec == MAX_SECTORS) audio_set_song(SONG_BOSS);
    else if (next_sec >= 3) audio_set_song(SONG_CYBER);
    else audio_set_song(SONG_EMERALD);
  }
  if (key_pressed['t']){
    g_mode = (g_mode == MODE_FORGE) ? MODE_CAMPAIGN : MODE_FORGE;
    if (g_mode != MODE_FORGE) g_player.speed = 0x3800;
    audio_set_song(g_mode == MODE_FORGE ? SONG_FORGE : SONG_EMERALD);
    set_status_banner(g_mode == MODE_FORGE
                      ? "HOVER DRONE: WASD MOVE, LMB SCULPT"
                      : "FLIGHT COMBAT MODE ENGAGED!", 110);
  }

  if (g_mode == MODE_FORGE){
    for (int k = 0; k < TOOL_COUNT; ++k){
      if (key_pressed['1' + k]){
        g_player.forge_tool = k;
        sfx_trigger(SFX_RING_PASS);
      }
    }
    if (mouse_wheel_delta != 0){
      g_player.forge_radius += (mouse_wheel_delta > 0) ? 1 : -1;
      if (g_player.forge_radius < 2) g_player.forge_radius = 2;
      if (g_player.forge_radius > 8) g_player.forge_radius = 8;
    }
    if (key_pressed['['] && g_player.forge_radius > 2) g_player.forge_radius -= 1;
    if (key_pressed[']'] && g_player.forge_radius < 8) g_player.forge_radius += 1;
    if (key_pressed['b']){
      g_biome = (g_biome + 1) % BIOME_COUNT;
      set_status_banner("BIOME PALETTE CYCLED!", 75);
      sfx_trigger(SFX_RING_PASS);
    }
    if (key_pressed['o']){
      bool ok = world_save_file(NULL);
      set_status_banner(ok ? "3D WORLD SAVED TO /skyforge/world.sf" : "WORLD SAVE FAILED", 100);
      sfx_trigger(SFX_BEACON_PICKUP);
    }
    if (key_pressed['l']){
      bool ok = world_load_file(NULL);
      set_status_banner(ok ? "3D WORLD LOADED FROM /skyforge/world.sf" : "NO SAVED WORLD FOUND", 100);
      sfx_trigger(SFX_RING_PASS);
    }
  }

  if (g_autopilot){
    if (key_held['w'] || key_held['s'] || key_held['a'] || key_held['d'] ||
        key_held[KEY_UP] || key_held[KEY_DOWN] || key_held[KEY_LEFT] || key_held[KEY_RIGHT]){
      g_autopilot = false;
      set_status_banner("MANUAL FLIGHT CONTROL", 60);
    } else {
      autopilot_decide(ctrl);
      return;
    }
  }

  if (g_mode == MODE_FORGE){
    /* Hover-Drone controls: WASD moves over map, Q/E or Left/Right turns */
    if (key_held['w'] || key_held[KEY_UP])   ctrl->throttle = 1;
    if (key_held['s'] || key_held[KEY_DOWN]) ctrl->throttle = -1;
    if (key_held['a']) ctrl->strafe = -1;
    if (key_held['d']) ctrl->strafe = 1;
    if (key_held['q'] || key_held[KEY_LEFT])  ctrl->turn = -8;
    if (key_held['e'] || key_held[KEY_RIGHT]) ctrl->turn = 8;
    if (key_held['z']) ctrl->pitch = -2;
    if (key_held['c']) ctrl->pitch = 2;
    if (key_held[' '] || (mouse_buttons & MOUSE_BUTTON_LEFT)){
      ctrl->sculpt_apply = 1;
    }
    if (key_held['x'] || (mouse_buttons & MOUSE_BUTTON_RIGHT)){
      ctrl->sculpt_erase = 1;
    }
    return;
  }

  /* Flight Mode controls: keyboard banking + mouse-aim turn assist */
  int kb_turn = 0;
  if (key_held['a'] || key_held['q'] || key_held[KEY_LEFT])  kb_turn -= 10;
  if (key_held['d'] || key_held[KEY_RIGHT])                  kb_turn += 10;
  ctrl->turn = kb_turn + (g_player.aim_x >> 4);

  int kb_pitch = 0;
  if (key_held['w'] || key_held[KEY_UP])   kb_pitch += 8;
  if (key_held['s'] || key_held[KEY_DOWN]) kb_pitch -= 8;
  ctrl->pitch = kb_pitch - (g_player.aim_y >> 4);

  if (key_held[KEY_LEFT_SHIFT] || key_held[KEY_RIGHT_SHIFT]) ctrl->throttle = 1;
  else if (key_held['z'] || key_held['c']) ctrl->throttle = -1;

  if (key_pressed['e'] || (mouse_clicked & MOUSE_BUTTON_MIDDLE)){
    ctrl->barrel_roll = 1;
  }
  if (key_held[' '] || (mouse_buttons & MOUSE_BUTTON_LEFT)){
    ctrl->fire_primary = 1;
  }
  if (key_pressed['f'] || (mouse_clicked & MOUSE_BUTTON_RIGHT)){
    ctrl->fire_seismic = 1;
  }
}

/* Run the deterministic headless guest engine and MMIO check (--selftest). */
static int run_guest_selftest(void){
  math_init();
  render_init();

  for (int sec = 1; sec <= MAX_SECTORS; ++sec){
    world_generate(sec, MODE_CAMPAIGN, 0x5A17F04Eu);
    if (world_enemies_remaining() <= 0){
      puts("***FAIL skyforge enemy spawn\n");
      return 1;
    }
  }

  world_generate(1, MODE_FORGE, 0x5A17F04Eu);
  int h_before = terrain_height_cell(64, 64);
  forge_apply_tool(64, 64, TOOL_RAISE, 4, false);
  int h_raised = terrain_height_cell(64, 64);
  if (h_raised <= h_before){
    puts("***FAIL skyforge terrain raise\n");
    return 1;
  }
  terrain_deform(64, 64, 5, -12, 1);
  int h_crater = terrain_height_cell(64, 64);
  if (h_crater >= h_raised){
    puts("***FAIL skyforge crater deform\n");
    return 1;
  }

  world_generate(1, MODE_CAMPAIGN, 0x5A17F04Eu);
  g_state = STATE_PLAY;
  struct FlightControls ctrl;
  for (int t = 0; t < 60; ++t){
    autopilot_decide(&ctrl);
    ctrl.fire_primary = (t & 7) == 0;
    ctrl.fire_seismic = (t == 10);
    world_tick(&ctrl);
  }

  struct Camera cam;
  cam.x = g_player.x;
  cam.y = g_player.y;
  cam.z = g_player.z;
  cam.angle = g_player.angle;
  cam.pitch = g_player.pitch;
  cam.roll = g_player.roll;
  cam.biome = g_biome;
  render_scene(&cam);
  unsigned hash1 = render_view_checksum();
  if (hash1 == 0 || hash1 == 2166136261u){
    puts("***FAIL skyforge render checksum\n");
    return 1;
  }

  world_generate(1, MODE_RACE, 0x5A17F04Eu);
  for (int t = 0; t < 30; ++t){
    autopilot_decide(&ctrl);
    world_tick(&ctrl);
  }

  g_pixel_fb = get_pixel_fb();
  g_tile_fb = get_tile_fb();
  g_tilemap = get_tilemap();
  g_spritemap = get_spritemap();
  if (g_pixel_fb == (volatile short*)-1 || g_tile_fb == (volatile short*)-1 ||
      g_tilemap == (volatile short*)-1 || g_spritemap == (volatile short*)-1){
    puts("***FAIL skyforge display mappings\n");
    return 1;
  }
  hud_init();
  blit_strip_groups(g_pixel_fb, g_view_words, 0, VIEW_GROUPS);
  hud_draw_panel();
  hud_draw_overlays(0);
  hud_hide_sprites();

  short pcm[250];
  audio_synth_test_chunk(pcm, 250);
  bool any_nonzero = false;
  for (int i = 0; i < 250; ++i){
    if (pcm[i] != 0) any_nonzero = true;
  }
  if (!any_nonzero){
    puts("***FAIL skyforge audio synth\n");
    return 1;
  }
  if (audio_open() == 0){
    int wr = audio_write(pcm, 500);
    audio_close();
    if (wr <= 0){
      puts("***FAIL skyforge audio write\n");
      return 1;
    }
  }

  g_terrain[100] = 0x4F2Au;
  if (!world_save_file("test_world.sf")){
    puts("***FAIL skyforge world save\n");
    return 1;
  }
  g_terrain[100] = 0;
  if (!world_load_file("test_world.sf")){
    puts("***FAIL skyforge world load\n");
    return 1;
  }
  unlink("test_world.sf");
  if ((g_terrain[100] & 0x3F) != (0x4F2Au & 0x3F)){
    puts("***FAIL skyforge world roundtrip\n");
    return 1;
  }

  puts("***PASS skyforge guest engine\n");
  return 0;
}

/* Run a 120-frame simulation and 3D voxel rendering benchmark (--bench). */
static int run_benchmark(void){
  math_init();
  render_init();
  world_generate(1, MODE_CAMPAIGN, 0x5A17F04Eu);
  g_state = STATE_PLAY;
  struct FlightControls ctrl;
  struct Camera cam;

  unsigned t0 = get_current_jiffies();
  for (int f = 0; f < 120; ++f){
    autopilot_decide(&ctrl);
    world_tick(&ctrl);
    cam.x = g_player.x;
    cam.y = g_player.y;
    cam.z = g_player.z;
    cam.angle = g_player.angle;
    cam.pitch = g_player.pitch;
    cam.roll = g_player.roll;
    cam.biome = g_biome;
    render_scene(&cam);
  }
  unsigned dt = get_current_jiffies() - t0;
  unsigned args[2];
  args[0] = dt;
  args[1] = render_view_checksum();
  printf("SKYFORGE 120-frame 3D Voxel Benchmark: %u jiffies (checksum %x)\n", args);
  return 0;
}

/* Main entry point for the SKYFORGE user program. */
int main(int argc, char** argv){
  bool opt_silent = false;
  bool opt_demo = false;

  for (int i = 1; i < argc; ++i){
    if (strcmp(argv[i], "--selftest") == 0){
      return run_guest_selftest();
    } else if (strcmp(argv[i], "--bench") == 0){
      return run_benchmark();
    } else if (strcmp(argv[i], "--silent") == 0){
      opt_silent = true;
    } else if (strcmp(argv[i], "--demo") == 0){
      opt_demo = true;
    } else if (strncmp(argv[i], "seed=", 5) == 0){
      g_seed = parse_uint(argv[i] + 5);
    }
  }

  g_pixel_fb = get_pixel_fb();
  g_tile_fb = get_tile_fb();
  g_tilemap = get_tilemap();
  g_spritemap = get_spritemap();
  if (g_pixel_fb == (volatile short*)-1 || g_tile_fb == (volatile short*)-1 ||
      g_tilemap == (volatile short*)-1 || g_spritemap == (volatile short*)-1){
    puts("skyforge: failed to map VGA display regions\n");
    return 1;
  }

  write(STDOUT, "\x1b[?25l", 6);
  if (set_keyboard_raw(1) == 0){
    raw_kb_active = true;
  }

  request_priority(DIOPTASE_PRIORITY_HIGH);
  pin_core(CORE_GAME);

  math_init();
  render_init();
  scores_load();
  hud_init();
  audio_start(opt_silent);

  if (opt_demo){
    g_demo_mode = false;
    g_autopilot = true;
    g_state = STATE_PLAY;
    world_generate(1, MODE_CAMPAIGN, g_seed);
    audio_set_song(SONG_EMERALD);
  } else {
    g_demo_mode = true;
    g_autopilot = true;
    g_state = STATE_TITLE;
    world_generate(1, MODE_CAMPAIGN, g_seed);
    audio_set_song(SONG_TITLE);
  }

  unsigned last_vga = get_vga_frame_counter();
  struct FlightControls ctrl;
  struct Camera cam;

  while (app_running){
    poll_inputs();
    update_controls(&ctrl);
    if (!app_running) break;

    if (g_state == STATE_PLAY || g_state == STATE_TITLE || g_state == STATE_HELP){
      world_tick(&ctrl);
    }

    int shake_x = 0;
    int shake_y = 0;
    if (g_player.shake > 0){
      shake_x = ((g_frame & 1) ? g_player.shake : -g_player.shake) >> 1;
      shake_y = ((g_frame & 2) ? g_player.shake : -g_player.shake) >> 1;
    }
    cam.x = g_player.x;
    cam.y = g_player.y;
    cam.z = g_player.z;
    cam.angle = (g_player.angle + shake_x) & ANGLE_MASK;
    cam.pitch = g_player.pitch + shake_y;
    cam.roll = g_player.roll;
    cam.biome = g_biome;

    render_scene(&cam);
    blit_strip_groups(g_pixel_fb, g_view_words, 0, VIEW_GROUPS);
    hud_draw_panel();
    hud_update_sprites();
    hud_draw_overlays(menu_cursor);
    audio_heartbeat();

    unsigned now_vga = get_vga_frame_counter();
    if (now_vga == last_vga){
      unsigned wait_start = get_current_jiffies();
      while (get_vga_frame_counter() == last_vga){
        if (get_current_jiffies() - wait_start > 60) break;
        yield();
      }
      last_vga = get_vga_frame_counter();
    } else {
      last_vga = now_vga;
    }
  }

  hud_hide_sprites();
  audio_stop();
  if (raw_kb_active){
    set_keyboard_raw(0);
  }
  write(STDOUT, "\x1b[?25h", 6);
  return 0;
}
