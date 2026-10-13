#include "realm.h"
#include "assets.h"

#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/print.h"
#include "../crt/string.h"
#include "../crt/mouse.h"

#define HANDOFF_FRAMES 6
#define VGA_PROBE_JIFFIES 3000
#define INPUT_EVENTS_PER_FRAME 16

/* Global world instance in static memory outside limited stack frame. */
static struct GameWorld world;

/* Mouse hardware position tracking in 320x240 coordinates. */
static int mouse_x = 160;
static int mouse_y = 120;
static int mouse_raw_x = 320;
static int mouse_raw_y = 240;
static int prev_mouse_buttons = 0;
static int drag_active = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;

/* Read next pending keyboard event byte or non-blocking character. */
static int next_key(void) {
  int available = fd_bytes_available(STDIN);
  if (available > 0) {
    unsigned char byte = 0;
    if (read(STDIN, &byte, 1) == 1) return (int)byte;
    return -1;
  }
  if (available < 0) {
    int ev = getkey();
    if (ev == 0) return -1;
    if (ev & 0x100) return -1; /* ignore release */
    return ev & 255;
  }
  return -1;
}

/* Consume any pending input bytes left in the terminal pipeline. */
static void drain_keys(void) {
  for (int i = 0; i < 256; i++) {
    if (next_key() < 0) break;
  }
}

/* Poll mouse delta packets and update cursor coordinates. */
static void poll_mouse(void) {
  while (1) {
    int ev = getmouse();
    if (ev == 0) break;

    int dx = MOUSE_DX(ev);
    int dy = MOUSE_DY(ev);

    mouse_raw_x += dx;
    mouse_raw_y += dy;

    if (mouse_raw_x < 0) mouse_raw_x = 0;
    if (mouse_raw_x >= (SCREEN_W * 2)) mouse_raw_x = (SCREEN_W * 2) - 1;
    if (mouse_raw_y < 0) mouse_raw_y = 0;
    if (mouse_raw_y >= (MAX_RENDER_Y * 2)) mouse_raw_y = (MAX_RENDER_Y * 2) - 1;

    mouse_x = mouse_raw_x / 2;
    mouse_y = mouse_raw_y / 2;

    int cur_buttons = MOUSE_BUTTONS(ev);

    /* Left button pressed down edge */
    if ((cur_buttons & MOUSE_BUTTON_LEFT) && !(prev_mouse_buttons & MOUSE_BUTTON_LEFT)) {
      if (world.game_state == STATE_PLAYING) {
        if (mouse_y >= HUD_Y) {
          /* Minimap click navigation */
          if (mouse_x >= 3 && mouse_x <= 27 && mouse_y >= HUD_Y + 2 && mouse_y <= HUD_Y + 26) {
            int mm_x = mouse_x - 3;
            int mm_y = mouse_y - (HUD_Y + 2);
            world.cam_x = (mm_x * WORLD_PX / 24) - (SCREEN_W / 2);
            world.cam_y = (mm_y * WORLD_PX / 24) - (VIEW_H / 2);
            if (world.cam_x < 0) world.cam_x = 0;
            if (world.cam_y < 0) world.cam_y = 0;
            if (world.cam_x > WORLD_PX - SCREEN_W) world.cam_x = WORLD_PX - SCREEN_W;
            if (world.cam_y > WORLD_PX - VIEW_H) world.cam_y = WORLD_PX - VIEW_H;
          }
        } else {
          if (world.selected_building_type != BLD_NONE) {
            /* Try building placement */
            int gtx = (mouse_x + world.cam_x) / TILE_PX;
            int gty = (mouse_y + world.cam_y) / TILE_PX;
            if (world_place_building(&world, world.selected_building_type, gtx, gty) >= 0) {
              world.selected_building_type = BLD_NONE;
            }
          } else {
            /* Start potential drag selection */
            drag_active = 1;
            drag_start_x = mouse_x;
            drag_start_y = mouse_y;
          }
        }
      }
    }

    /* Left button released edge */
    if (!(cur_buttons & MOUSE_BUTTON_LEFT) && (prev_mouse_buttons & MOUSE_BUTTON_LEFT)) {
      if (drag_active) {
        drag_active = 0;
        int ddx = mouse_x - drag_start_x;
        int ddy = mouse_y - drag_start_y;
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;

        if (ddx < 4 && ddy < 4) {
          /* Single click unit selection */
          int click_wx = mouse_x + world.cam_x;
          int click_wy = mouse_y + world.cam_y;

          for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
            world.units[u].selected = 0;
          }
          world.selected_count = 0;

          for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
            struct Unit* unit = &world.units[u];
            if (!unit->active || unit->team != TEAM_PLAYER || unit->hp <= 0) continue;
            int ux = unit->x >> FIXED_SHIFT;
            int uy = unit->y >> FIXED_SHIFT;
            if ((click_wx - ux) * (click_wx - ux) + (click_wy - uy) * (click_wy - uy) < (12 * 12)) {
              unit->selected = 1;
              world.selected_count = 1;
              break;
            }
          }
        } else {
          /* Box selection */
          int bx0 = (drag_start_x < mouse_x) ? drag_start_x : mouse_x;
          int by0 = (drag_start_y < mouse_y) ? drag_start_y : mouse_y;
          int bx1 = (drag_start_x < mouse_x) ? mouse_x : drag_start_x;
          int by1 = (drag_start_y < mouse_y) ? mouse_y : drag_start_y;

          bx0 += world.cam_x;
          bx1 += world.cam_x;
          by0 += world.cam_y;
          by1 += world.cam_y;

          for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
            world.units[u].selected = 0;
          }
          world.selected_count = 0;

          for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
            struct Unit* unit = &world.units[u];
            if (!unit->active || unit->team != TEAM_PLAYER || unit->hp <= 0) continue;
            int ux = unit->x >> FIXED_SHIFT;
            int uy = unit->y >> FIXED_SHIFT;
            if (ux >= bx0 && ux <= bx1 && uy >= by0 && uy <= by1) {
              unit->selected = 1;
              world.selected_count++;
            }
          }
        }
      }
    }

    /* Right button clicked */
    if ((cur_buttons & MOUSE_BUTTON_RIGHT) && !(prev_mouse_buttons & MOUSE_BUTTON_RIGHT)) {
      if (world.selected_building_type != BLD_NONE) {
        world.selected_building_type = BLD_NONE;
      } else if (world.selected_count > 0 && mouse_y < VIEW_H) {
        int r_wx = mouse_x + world.cam_x;
        int r_wy = mouse_y + world.cam_y;
        int r_tx = r_wx / TILE_PX;
        int r_ty = r_wy / TILE_PX;

        struct MapCell* cell = world_get_cell(&world, r_tx, r_ty);

        for (int u = 0; u < MAX_VILLAGERS + MAX_SOLDIERS + MAX_ENEMIES; u++) {
          struct Unit* unit = &world.units[u];
          if (!unit->active || !unit->selected) continue;

          if (unit->type == UNIT_VILLAGER && cell != NULL && cell->resource != RES_NONE) {
            /* Gather order */
            unit->target_tile_x = r_tx;
            unit->target_tile_y = r_ty;
            unit->target_x = (r_tx * TILE_PX + 8) << FIXED_SHIFT;
            unit->target_y = (r_ty * TILE_PX + 8) << FIXED_SHIFT;
            unit->state = STATE_GATHER;
          } else {
            /* Walk order */
            unit->target_x = r_wx << FIXED_SHIFT;
            unit->target_y = r_wy << FIXED_SHIFT;
            unit->target_tile_x = -1;
            unit->target_tile_y = -1;
            unit->state = STATE_WALK;
          }
        }
        add_particle(&world, r_wx, r_wy, 0, 0, PAL_GRASS_LIGHT, 8);
      }
    }

    prev_mouse_buttons = cur_buttons;
  }
}

/* Process user key actions for command shortcuts, building, and camera panning. */
static void process_keyboard(bool* quit) {
  for (int i = 0; i < INPUT_EVENTS_PER_FRAME; i++) {
    int key = next_key();
    if (key < 0) break;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';

    if (key == 'q') {
      *quit = true;
      return;
    }

    if (world.game_state == STATE_TITLE) {
      if (key == '\n' || key == '\r') {
        world.game_state = STATE_PLAYING;
        world.auto_commander = 0;
      } else if (key == 'd') {
        world.game_state = STATE_PLAYING;
        world.auto_commander = 1;
      } else if (key == 'h') {
        world.game_state = STATE_HELP;
      }
    } else if (world.game_state == STATE_HELP) {
      world.game_state = STATE_TITLE;
    } else if (world.game_state == STATE_GAMEOVER || world.game_state == STATE_VICTORY) {
      if (key == '\n' || key == '\r' || key == 'r') {
        world_init(&world, 98765);
        world.game_state = STATE_PLAYING;
      }
    } else if (world.game_state == STATE_PLAYING) {
      if (key == ' ') {
        world.game_state = STATE_PAUSED;
      } else if (key == 'd') {
        world.auto_commander = !world.auto_commander;
      } else if (key == 'm') {
        world.sound_enabled = !world.sound_enabled;
      } else if (key == 'h') {
        world.selected_building_type = BLD_HOUSE;
      } else if (key == 'f') {
        world.selected_building_type = BLD_FARM;
      } else if (key == 't') {
        world.selected_building_type = BLD_TOWER;
      } else if (key == 'b') {
        world.selected_building_type = BLD_BARRACKS;
      } else if (key == 'w') {
        world.selected_building_type = BLD_WALL;
      } else if (key == 'v') {
        if (world.food >= 40 && world.population < world.max_population) {
          world.food -= 40;
          unit_spawn(&world, UNIT_VILLAGER, TEAM_PLAYER, (31 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
          add_float_text(&world, 31 * TILE_PX, 33 * TILE_PX, "+CITIZEN", PAL_GOLD_BRIGHT);
        }
      } else if (key == 's') {
        if (world.food >= 50 && world.gold >= 25 && world.population < world.max_population) {
          world.food -= 50;
          world.gold -= 25;
          unit_spawn(&world, UNIT_SWORDSMAN, TEAM_PLAYER, (32 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
          add_float_text(&world, 32 * TILE_PX, 33 * TILE_PX, "+KNIGHT", PAL_BANNER_BLUE);
        }
      } else if (key == 'a') {
        if (world.food >= 45 && world.gold >= 30 && world.population < world.max_population) {
          world.food -= 45;
          world.gold -= 30;
          unit_spawn(&world, UNIT_ARCHER, TEAM_PLAYER, (32 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
          add_float_text(&world, 32 * TILE_PX, 33 * TILE_PX, "+ARCHER", PAL_LEAVES_DEEP);
        }
      }
    } else if (world.game_state == STATE_PAUSED) {
      if (key == ' ') {
        world.game_state = STATE_PLAYING;
      }
    }
  }

  /* Camera edge scrolling or keyboard panning */
  if (world.game_state == STATE_PLAYING) {
    if (mouse_x < 8) world.cam_x -= 8;
    if (mouse_x > SCREEN_W - 8) world.cam_x += 8;
    if (mouse_y < 8) world.cam_y -= 8;
    if (mouse_y > VIEW_H - 8 && mouse_y < VIEW_H) world.cam_y += 8;

    if (world.cam_x < 0) world.cam_x = 0;
    if (world.cam_y < 0) world.cam_y = 0;
    if (world.cam_x > WORLD_PX - SCREEN_W) world.cam_x = WORLD_PX - SCREEN_W;
    if (world.cam_y > WORLD_PX - VIEW_H) world.cam_y = WORLD_PX - VIEW_H;
  }
}

/* Establish ownership of video framebuffer and hide terminal cursor. */
static bool hand_off_display(void) {
  puts("\x1b[?25l\x1b[2J");
  unsigned frame = get_vga_frame_counter();
  unsigned start = get_current_jiffies();
  while (get_vga_frame_counter() - frame < HANDOFF_FRAMES) {
    if (get_current_jiffies() - start > VGA_PROBE_JIFFIES) {
      return false;
    }
    sleep(1);
  }
  return true;
}

/* Headless self-test verifying game invariants, rules, and economy for CI. */
static int run_selftest(void) {
  world_init(&world, 0x4D494352);

  if (world.wood != 140 || world.stone != 80 || world.food != 90) {
    puts("***FAIL microrealm initial stockpile invariants\n");
    return 1;
  }

  if (world.population != 3) {
    puts("***FAIL microrealm initial population census\n");
    return 1;
  }

  /* Test building placement */
  int house_id = world_place_building(&world, BLD_HOUSE, 28, 28);
  if (house_id < 0 || world.max_population != 15 || world.wood != 100) {
    puts("***FAIL microrealm house construction or pop boost\n");
    return 1;
  }

  /* Test villager production */
  int new_unit = unit_spawn(&world, UNIT_VILLAGER, TEAM_PLAYER, (31 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
  if (new_unit < 0 || world.population != 4) {
    puts("***FAIL microrealm unit spawning\n");
    return 1;
  }

  /* Test simulation ticks */
  for (int t = 0; t < 60; t++) {
    world_update(&world);
    units_update(&world);
    projectiles_update(&world);
    particles_update(&world);
    float_texts_update(&world);
  }

  /* Test combat resolution */
  int goblin_id = unit_spawn(&world, UNIT_GOBLIN, TEAM_ENEMY, (31 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
  int knight_id = unit_spawn(&world, UNIT_SWORDSMAN, TEAM_PLAYER, (31 * TILE_PX) << FIXED_SHIFT, (34 * TILE_PX) << FIXED_SHIFT);
  if (goblin_id < 0 || knight_id < 0) {
    puts("***FAIL microrealm combat spawning\n");
    return 1;
  }

  for (int t = 0; t < 50; t++) {
    units_update(&world);
    projectiles_update(&world);
  }

  /* A new campaign owns its harvest clock. Resetting after earlier updates
   * must not inherit a static timer; harvest remains tied to day_ticks,
   * including the shorter interval from the final daily harvest to midnight.
   */
  world_init(&world, 0x4D494352);
  if (world_place_building(&world, BLD_FARM, 28, 28) < 0) {
    puts("***FAIL microrealm farm clock test could not place farm at tile (28,28)\n");
    return 1;
  }
  for (int t = 0; t < 89; t++) world_update(&world);
  if (world.food != 90) {
    puts("***FAIL microrealm new campaign harvested before day tick 90\n");
    return 1;
  }
  world_update(&world);
  if (world.food != 94) {
    puts("***FAIL microrealm farm did not yield four food at day tick 90\n");
    return 1;
  }
  world.day_ticks = 1199;
  world.phase = PHASE_NIGHT;
  world_update(&world);
  if (world.day_ticks != 0 || world.food != 98) {
    puts("***FAIL microrealm midnight must reset the day clock and harvest four food\n");
    return 1;
  }

  /* Test persistence */
  world.record.best_days = 5;
  world.record.best_score = 3500;
  world.record.best_kills = 24;
  save_write_record(&world.record);

  struct HighScore verify;
  save_load_record(&verify);
  if (verify.best_score != 3500 || verify.best_days != 5) {
    puts("***FAIL microrealm record persistence\n");
    return 1;
  }

  puts("***PASS microrealm guest engine\n");
  return 0;
}

/* Main program entry executing game loop, event polling, and rendering. */
int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
    return run_selftest();
  }

  save_load_record(&world.record);
  world_init(&world, 54321);
  save_load_record(&world.record);

  if (argc > 1 && strcmp(argv[1], "--demo") == 0) {
    world.game_state = STATE_PLAYING;
    world.auto_commander = 1;
  }

  bool vga_present = hand_off_display();
  if (vga_present) {
    render_init();
  }
  audio_init();
  drain_keys();

  bool quit = false;
  unsigned prev_frame = get_vga_frame_counter();

  while (!quit) {
    poll_mouse();
    process_keyboard(&quit);

    if (world.game_state == STATE_PLAYING) {
      if (world.auto_commander) {
        ai_commander_step(&world);
      }
      world_update(&world);
      units_update(&world);
      projectiles_update(&world);
      particles_update(&world);
      float_texts_update(&world);
    }

    audio_service(&world);

    if (vga_present) {
      if (world.game_state == STATE_TITLE) {
        render_title_screen(&world);
      } else if (world.game_state == STATE_HELP) {
        render_help_screen();
      } else if (world.game_state == STATE_GAMEOVER) {
        render_gameover_screen(&world);
      } else if (world.game_state == STATE_VICTORY) {
        render_victory_screen(&world);
      } else {
        render_frame(&world, mouse_x, mouse_y, drag_active, drag_start_x, drag_start_y);
      }

      /* Wait for next presentation frame */
      unsigned cur;
      do {
        sleep(1);
        cur = get_vga_frame_counter();
      } while (cur == prev_frame);
      prev_frame = cur;
    } else {
      sleep(1);
    }
  }

  /* Update and persist records upon clean exit */
  if (world.day_number > world.record.best_days) world.record.best_days = world.day_number;
  if (world.score > world.record.best_score) world.record.best_score = world.score;
  if (world.enemies_slain > world.record.best_kills) world.record.best_kills = world.enemies_slain;
  save_write_record(&world.record);

  audio_shutdown();
  puts("\x1b[?25h\x1b[2J");
  return 0;
}
