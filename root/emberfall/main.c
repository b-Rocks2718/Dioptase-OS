/* Emberfall entry: terminal handoff, 30 Hz loop, menus, and selftest.
 *
 * Display ownership follows snake and dino: hide the cursor and clear through
 * stdout, wait for the terminal, then own the tile and pixel layers until
 * exit. The shell restores the terminal afterwards. Game ticks run every two
 * VGA frames so wall-clock pacing survives emulator speed changes.
 */

#include "emberfall.h"
#include "render.h"
#include "input.h"
#include "audio.h"
#include "save.h"

#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/string.h"

#define HANDOFF_FRAMES 10
#define VGA_PROBE_JIFFIES 3000

static struct Game game;
static struct InputState input;
static struct HighScores scores;
static int muted = 0;
static int last_phase = -1;
static int gameover_saved = 0;
static int gameover_new_best = 0;

/* Wait until the VGA frame counter advances past the given value. */
static void wait_frame(unsigned last) {
  while (get_vga_frame_counter() == last) {
  }
}

/* Headless guest check: rules, persistence format, and display mapping. */
static int run_selftest(void) {
  struct GameInput in;
  int i = 0;
  game_init(&game, 0xA5A5A5A5u, 0);
  if (game.phase != PHASE_TITLE) {
    puts("***FAIL emberfall selftest title phase\n");
    return 1;
  }
  in.move_x = 0;
  in.move_y = 0;
  in.start = 1;
  in.pause = 0;
  in.choose = -1;
  in.confirm = 0;
  game_tick(&game, in);
  if (game.phase != PHASE_PLAY || game.player.hp != 100) {
    puts("***FAIL emberfall selftest launch\n");
    return 1;
  }
  in.start = 0;
  in.move_x = 1;
  i = 0;
  while (i < 120) {
    game_tick(&game, in);
    if (game.phase != PHASE_PLAY) break;
    i++;
  }
  if (game.tick == 0) {
    puts("***FAIL emberfall selftest tick advance\n");
    return 1;
  }
  /* Force a level-up and take the first card. */
  game.player.xp = game.player.xp_next;
  in.move_x = 0;
  game_tick(&game, in);
  if (game.phase != PHASE_LEVELUP) {
    puts("***FAIL emberfall selftest levelup phase\n");
    return 1;
  }
  in.choose = 0;
  game_tick(&game, in);
  if (game.player.level != 2) {
    puts("***FAIL emberfall selftest upgrade apply\n");
    return 1;
  }
  /* Force death and check best tracking. */
  game.score = 777;
  game.player.hp = 1;
  game.player.invuln_timer = 0;
  game.enemies[0].active = 1;
  game.enemies[0].kind = ENEMY_SLIME;
  game.enemies[0].x = game.player.x;
  game.enemies[0].y = game.player.y;
  game.enemies[0].hp = 50;
  game.enemies[0].max_hp = 50;
  game.enemies[0].speed = 0;
  game.enemies[0].damage = 50;
  game.enemies[0].slow_timer = 0;
  game.enemies[0].hit_timer = 0;
  in.choose = -1;
  game_tick(&game, in);
  if (game.phase != PHASE_GAMEOVER || game.best != 777) {
    puts("***FAIL emberfall selftest gameover\n");
    return 1;
  }
  /* Display mappings must resolve without faulting. */
  {
    volatile short* px = get_pixel_fb();
    volatile short* tfb = get_tile_fb();
    volatile short* tm = get_tilemap();
    volatile short* sp = get_spritemap();
    if (px == (short*)-1 || tfb == (short*)-1 || tm == (short*)-1 ||
        sp == (short*)-1) {
      puts("***FAIL emberfall selftest display map\n");
      return 1;
    }
    px[0] = px[0];
    tfb[0] = tfb[0];
  }
  /* Raw mode round-trips; leave the terminal cooked. */
  if (set_keyboard_raw(1) != 0 || set_keyboard_raw(0) != 0) {
    puts("***FAIL emberfall selftest raw mode\n");
    return 1;
  }
  /* High-score insert keeps order. */
  {
    struct HighScores hs;
    int k = 0;
    while (k < 5) {
      hs.score[k] = 100 * (5 - k);
      hs.time_sec[k] = 10 * (5 - k);
      hs.kills[k] = 5 - k;
      hs.level[k] = 2;
      k++;
    }
    save_record(&hs, 450, 99, 9, 9);
    if (hs.score[0] != 500 || hs.score[1] != 450) {
      puts("***FAIL emberfall selftest scores\n");
      return 1;
    }
  }
  puts("***PASS emberfall guest engine\n");
  return 0;
}

/* Run selftest with --selftest, else the 30 Hz game loop. */
int main(int argc, char** argv) {
  unsigned seed = 0;
  unsigned frame = 0;
  int f = 0;
  if (argc > 1 && streq(argv[1], "--selftest")) {
    return run_selftest();
  }
  /* Require the VGA window: pacing follows the 60 Hz frame counter. */
  {
    unsigned before = get_vga_frame_counter();
    sleep(60);
    if (get_vga_frame_counter() == before) {
      puts("emberfall: needs VGA (run: make run EMU_VGA=yes, then emberfall)\n");
      return 1;
    }
  }
  save_load(&scores);
  seed = 0xEBFA11u ^ get_current_jiffies();
  game_init(&game, seed, scores.score[0]);
  puts("\x1b[?25l\x1b[2J");
  frame = get_vga_frame_counter();
  f = 0;
  while (f < HANDOFF_FRAMES) {
    wait_frame(frame);
    frame = get_vga_frame_counter();
    f++;
  }
  render_init();
  input_init(&input);
  audio_init();
  /* Fresh arena behind the title: the title darkens it anyway. */
  frame = get_vga_frame_counter();
  while (1) {
    struct GameInput in;
    /* Two VGA frames per 30 Hz tick. */
    wait_frame(frame);
    frame = get_vga_frame_counter();
    wait_frame(frame);
    frame = get_vga_frame_counter();
    input_poll(&input, game.phase, &in);
    if (input.quit) break;
    if (input.mute_toggle) muted = !muted;
    game_tick(&game, in);
    if (input.quit) break;
    if (game.phase == PHASE_PLAY) {
      if (last_phase != PHASE_PLAY) {
        /* Returning from a menu: rebuild the arena from scratch. */
        render_init();
      }
      render_frame(&game, muted);
      render_hide_cursor();
    } else if (game.phase == PHASE_TITLE) {
      if (last_phase != PHASE_TITLE) render_title(&game, &scores);
      render_cursor(input.mouse_x, input.mouse_y);
    } else if (game.phase == PHASE_HELP) {
      if (last_phase != PHASE_HELP) render_help();
      render_cursor(input.mouse_x, input.mouse_y);
    } else if (game.phase == PHASE_PAUSE) {
      if (last_phase != PHASE_PAUSE) render_pause();
      render_cursor(input.mouse_x, input.mouse_y);
    } else if (game.phase == PHASE_LEVELUP) {
      if (last_phase != PHASE_LEVELUP) render_levelup(&game);
      render_cursor(input.mouse_x, input.mouse_y);
    } else if (game.phase == PHASE_GAMEOVER) {
      if (last_phase != PHASE_GAMEOVER) {
        gameover_new_best = save_record(&scores, game.score,
          game_seconds(&game), game.kills, game.player.level);
        gameover_saved = 1;
        render_gameover(&game, &scores, gameover_new_best);
      }
      render_cursor(input.mouse_x, input.mouse_y);
    }
    if (game.phase != PHASE_GAMEOVER) gameover_saved = 0;
    last_phase = game.phase;
    audio_tick(game.sound, muted, game.phase == PHASE_PLAY);
  }
  set_keyboard_raw(0);
  audio_shutdown();
  return 0;
}
