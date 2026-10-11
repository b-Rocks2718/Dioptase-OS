/* Scripted matrix: movement and aliases, all casts, menus, outcomes.
 *
 * Every key travels as raw PS/2 press/release words through a stdin pipe, so
 * input_poll, game_tick, the renderer, and persistence all run exactly as in
 * play. Assertions read back game state, tile text, pixel words, and files.
 * Enemy and gem fixtures use direct struct writes for determinism; nothing
 * about the input, render, audio-skip, or save paths is stubbed. Audio is
 * intentionally silent here: the mixer needs wall-clock refills, while this
 * pass runs frames back-to-back for timing observation.
 */

#include "e2e.h"
#include "emberfall.h"
#include "render.h"
#include "input.h"
#include "save.h"

#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/unistd.h"
#include "../crt/fcntl.h"
#include "../crt/ps2.h"

#define E2E_SEED 0xE2E2u
#define E2E_NO_SPAWN 1000000
#define E2E_ENTER 0x0D
#define E2E_ESC 0x1B
#define E2E_PIXEL_WORDS 75520
#define E2E_TILEFB_WORDS 4800
#define E2E_TILEMAP_WORDS 16384
#define E2E_CHUNK 1024

static struct Game e2e_game;
static struct InputState e2e_input;
static struct HighScores e2e_scores;
static int e2e_wfd;
static int e2e_last_phase;
static int e2e_gameover_saved;
static char e2e_chunk[1024];

/* Report a script failure; the ***FAIL line breaks the PASS baseline. */
static int e2e_fail(char* what) {
  puts("***FAIL emberfall e2e ");
  puts(what);
  puts("\n");
  return 1;
}

/* Write exactly n bytes to the script pipe. */
static int e2e_write_all(char* buf, int n) {
  int off = 0;
  while (off < n) {
    int w = write(e2e_wfd, buf + off, n - off);
    if (w <= 0) return -1;
    off += w;
  }
  return 0;
}

/* Emit one raw PS/2 word with bit 8 marking release. */
static int e2e_raw(int code, int release) {
  char b[2];
  b[0] = (char)(code & 0xFF);
  b[1] = (char)(release ? 1 : 0);
  return e2e_write_all(b, 2);
}

/* Press a key and leave it held. */
static int e2e_press(int code) {
  return e2e_raw(code, 0);
}

/* Release a held key. */
static int e2e_release(int code) {
  return e2e_raw(code, 1);
}

/* Tap a menu key: press immediately followed by release. */
static int e2e_tap(int code) {
  if (e2e_press(code)) return -1;
  return e2e_release(code);
}

/* Replace fd 0 with a scripted pipe; keep the write end for raw words. */
static int e2e_wire_stdin(void) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  close(STDIN);
  if (dup(fds[0]) != STDIN) return -1;
  close(fds[0]);
  e2e_wfd = fds[1];
  return 0;
}

/* Poll real stdin bytes, tick once, and render for the current phase. */
static void e2e_frame(struct GameInput* seen) {
  struct GameInput in;
  input_poll(&e2e_input, e2e_game.phase, &in);
  if (seen) {
    seen->move_x = in.move_x;
    seen->move_y = in.move_y;
    seen->start = in.start;
    seen->pause = in.pause;
    seen->choose = in.choose;
    seen->confirm = in.confirm;
  }
  game_tick(&e2e_game, in);
  if (e2e_game.phase == PHASE_PLAY) {
    if (e2e_last_phase != PHASE_PLAY) render_restore_arena();
    render_frame(&e2e_game, 0);
  } else if (e2e_game.phase == PHASE_TITLE) {
    render_title(&e2e_game, &e2e_scores);
  } else if (e2e_game.phase == PHASE_HELP) {
    render_help();
  } else if (e2e_game.phase == PHASE_PAUSE) {
    render_pause();
  } else if (e2e_game.phase == PHASE_LEVELUP) {
    render_levelup(&e2e_game);
  } else if (e2e_game.phase == PHASE_GAMEOVER) {
    if (!e2e_gameover_saved) {
      save_record(&e2e_scores, e2e_game.score, game_seconds(&e2e_game),
        e2e_game.kills, e2e_game.player.level);
      e2e_gameover_saved = 1;
    }
    render_gameover(&e2e_game, &e2e_scores, 0);
  }
  e2e_last_phase = e2e_game.phase;
}

/* Empty all entity pools for deterministic scripted segments. */
static void e2e_clear_pools(void) {
  int i = 0;
  while (i < 48) {
    e2e_game.enemies[i].active = 0;
    i++;
  }
  i = 0;
  while (i < 64) {
    e2e_game.projectiles[i].active = 0;
    e2e_game.gems[i].active = 0;
    i++;
  }
  i = 0;
  while (i < 96) {
    e2e_game.particles[i].active = 0;
    i++;
  }
}

/* Plant one fixture enemy at a pixel offset from the warden. */
static void e2e_plant(int slot, int kind, int dx_px, int dy_px, int hp,
    int dmg) {
  e2e_game.enemies[slot].active = 1;
  e2e_game.enemies[slot].kind = kind;
  e2e_game.enemies[slot].x = e2e_game.player.x + dx_px * 256;
  e2e_game.enemies[slot].y = e2e_game.player.y + dy_px * 256;
  e2e_game.enemies[slot].hp = hp;
  e2e_game.enemies[slot].max_hp = hp;
  e2e_game.enemies[slot].speed = 0;
  e2e_game.enemies[slot].damage = dmg;
  e2e_game.enemies[slot].slow_timer = 0;
  e2e_game.enemies[slot].hit_timer = 0;
}

/* Count live projectiles of one spell kind. */
static int e2e_count_kind(int kind) {
  int n = 0;
  int i = 0;
  while (i < 64) {
    if (e2e_game.projectiles[i].active &&
        e2e_game.projectiles[i].kind == kind) {
      n++;
    }
    i++;
  }
  return n;
}

/* True when tile row y holds text as a low-byte substring. */
static int e2e_row_has(int y, char* text) {
  volatile short* tfb = get_tile_fb();
  int len = 0;
  int x = 0;
  while (text[len] != 0) len++;
  while (x + len <= 80) {
    int k = 0;
    while (k < len && (tfb[y * 80 + x + k] & 0xFF) == text[k]) k++;
    if (k == len) return 1;
    x++;
  }
  return 0;
}

/* FNV-1a over sampled pixel words; cheap live-rendering witness. */
static unsigned e2e_pixel_hash(void) {
  volatile short* px = get_pixel_fb();
  unsigned h = 0x811C9DC5u;
  int i = 0;
  while (i < 75520) {
    h ^= (unsigned)(px[i] & 0xFFFF);
    h *= 0x01000193u;
    i += 61;
  }
  return h;
}

/* Read one arena pixel word from the live pixel framebuffer. */
static int e2e_pixel_at(int x, int y) {
  volatile short* px = get_pixel_fb();
  return px[y * 320 + x] & 0xFFFF;
}

/* Count pixel words holding the transparent marker bits (must stay 0). */
static int e2e_count_marker(void) {
  volatile short* px = get_pixel_fb();
  int n = 0;
  int i = 0;
  while (i < 75520) {
    if ((px[i] & 0xFFFF) == 0xF000) n++;
    i++;
  }
  return n;
}

/* Count pixels of one exact color word. */
static int e2e_count_color(short color) {
  volatile short* px = get_pixel_fb();
  int want = color & 0xFFFF;
  int n = 0;
  int i = 0;
  while (i < 75520) {
    if ((px[i] & 0xFFFF) == want) n++;
    i++;
  }
  return n;
}

/* Hold a movement key, asserting exact per-frame deltas and polled input. */
static int e2e_hold(int code, int n, int dx, int dy) {
  struct GameInput seen;
  int ex = 0;
  int ey = 0;
  int i = 0;
  if (dx > 0) ex = 1;
  if (dx < 0) ex = -1;
  if (dy > 0) ey = 1;
  if (dy < 0) ey = -1;
  if (e2e_press(code)) return e2e_fail("write press");
  while (i < n) {
    int x = e2e_game.player.x;
    int y = e2e_game.player.y;
    e2e_frame(&seen);
    if (e2e_game.phase != PHASE_PLAY) return e2e_fail("hold left play");
    if (e2e_game.player.x - x != dx) return e2e_fail("hold dx");
    if (e2e_game.player.y - y != dy) return e2e_fail("hold dy");
    if (seen.move_x != ex) return e2e_fail("hold input x");
    if (seen.move_y != ey) return e2e_fail("hold input y");
    i++;
  }
  if (e2e_release(code)) return e2e_fail("write release");
  {
    int x = e2e_game.player.x;
    int y = e2e_game.player.y;
    e2e_frame(&seen);
    if (seen.move_x != 0) return e2e_fail("release clears x");
    if (seen.move_y != 0) return e2e_fail("release clears y");
    if (e2e_game.player.x != x) return e2e_fail("release drift x");
    if (e2e_game.player.y != y) return e2e_fail("release drift y");
  }
  return 0;
}

/* Run frames until a spell kind appears, or fail past the cap. */
static int e2e_until_cast(int kind, int want, int cap) {
  int i = 0;
  while (i < cap) {
    e2e_frame(0);
    if (e2e_count_kind(kind) >= want) return 0;
    i++;
  }
  return e2e_fail("cast wait");
}

/* Write one unsigned decimal to a file. */
static void e2e_put_dec(int fd, unsigned v) {
  char tmp[10];
  int n = 0;
  if (v == 0) {
    write(fd, "0", 1);
    return;
  }
  while (v > 0 && n < 10) {
    tmp[n] = (char)('0' + v % 10);
    v /= 10;
    n++;
  }
  while (n > 0) {
    n--;
    write(fd, tmp + n, 1);
  }
}

/* Write one unsigned hex word to a file. */
static void e2e_put_hex(int fd, unsigned v) {
  int i = 7;
  while (i >= 0) {
    int d = (v >> (i * 4)) & 0xF;
    char c = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    write(fd, &c, 1);
    i--;
  }
}

/* Dump one MMIO word range to a new file in 1024-byte chunks. */
static int e2e_dump(char* path, volatile short* base, int words) {
  int fd = open(path);
  int off = 0;
  int bytes = words * 2;
  if (fd < 0) return -1;
  while (off < bytes) {
    int n = bytes - off;
    int i = 0;
    if (n > 1024) n = 1024;
    while (i < n) {
      int w = (off + i) / 2;
      int bit = base[w] & 0xFFFF;
      if (((off + i) & 1) == 0) e2e_chunk[i] = (char)(bit & 0xFF);
      else e2e_chunk[i] = (char)((bit >> 8) & 0xFF);
      i++;
    }
    if (write(fd, e2e_chunk, n) != n) {
      close(fd);
      return -1;
    }
    off += n;
  }
  close(fd);
  return 0;
}

/* Find a wanted upgrade kind among the three cards, or -1. */
static int e2e_find_choice(int want) {
  int i = 0;
  while (i < 3) {
    if (e2e_game.choices[i].kind == want) return i;
    i++;
  }
  return -1;
}

/* Drive the full scripted matrix in one pass. */
int run_e2e(void) {
  struct GameInput seen;
  unsigned h0 = 0;
  unsigned h1 = 0;
  unsigned perf_hash[5];
  unsigned frames = 0;
  unsigned j_total = 0;
  unsigned j_max = 0;
  int f000 = 0;
  int faces = 0;
  int i = 0;
  int tries = 0;
  if (e2e_wire_stdin()) return e2e_fail("stdin pipe");
  save_load(&e2e_scores);
  game_init(&e2e_game, E2E_SEED, e2e_scores.score[0]);
  render_init();
  input_init(&e2e_input);
  e2e_last_phase = -1;
  e2e_gameover_saved = 0;
  /* Title screenshot and signature line. */
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_TITLE) return e2e_fail("boot title");
  if (!e2e_row_has(8, "Warden")) return e2e_fail("title warden");
  if (!e2e_row_has(12, "muse-spark")) return e2e_fail("title signature");
  /* Help round-trip through real keys. */
  if (e2e_tap('h')) return e2e_fail("write h");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_HELP) return e2e_fail("help enter");
  if (!e2e_row_has(4, "HOW TO PLAY")) return e2e_fail("help text");
  if (e2e_tap(E2E_ENTER)) return e2e_fail("write enter");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_TITLE) return e2e_fail("help back");
  /* Start and HUD screenshot. */
  if (e2e_tap(E2E_ENTER)) return e2e_fail("write start");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_PLAY) return e2e_fail("start");
  if (!e2e_row_has(0, "HP")) return e2e_fail("hud hp");
  e2e_game.spawn_timer = E2E_NO_SPAWN;
  e2e_game.boss_timer = E2E_NO_SPAWN;
  h0 = e2e_pixel_hash();
  /* Movement matrix: WASD plus arrow aliases, exact fixed-point steps. */
  if (e2e_hold('w', 10, 0, -512)) return 1;
  frames += 11;
  h1 = e2e_pixel_hash();
  /* Face pixel followed the warden up; the old center is meadow again. */
  if (e2e_pixel_at(160, 98) != 0xF9B) return e2e_fail("warden pixel");
  if (e2e_pixel_at(160, 118) == 0xF9B) return e2e_fail("warden erase");
  if (e2e_hold('s', 10, 0, 512)) return 1;
  frames += 11;
  if (e2e_hold('a', 10, -512, 0)) return 1;
  frames += 11;
  if (e2e_hold('d', 10, 512, 0)) return 1;
  frames += 11;
  if (e2e_hold(KEY_UP, 5, 0, -512)) return 1;
  frames += 6;
  if (e2e_hold(KEY_DOWN, 5, 0, 512)) return 1;
  frames += 6;
  if (e2e_hold(KEY_LEFT, 5, -512, 0)) return 1;
  frames += 6;
  if (e2e_hold(KEY_RIGHT, 5, 512, 0)) return 1;
  frames += 6;
  if (e2e_game.player.x != 160 * 256) return e2e_fail("matrix home x");
  if (e2e_game.player.y != 118 * 256) return e2e_fail("matrix home y");
  /* Ember cast with sound. */
  e2e_clear_pools();
  e2e_plant(0, ENEMY_SLIME, 40, 0, 5000, 0);
  {
    int saw = 0;
    i = 0;
    while (i < 20 && e2e_count_kind(SPELL_EMBER) < 1) {
      e2e_frame(0);
      frames++;
      saw |= e2e_game.sound;
      i++;
    }
    if (e2e_count_kind(SPELL_EMBER) != 1) return e2e_fail("ember cast");
    if ((saw & SOUND_SHOOT) == 0) return e2e_fail("ember sound");
  }
  /* Earn fireball and frost through real level-up card picks. */
  tries = 0;
  while ((e2e_game.player.spell_level[SPELL_FIREBALL] == 0 ||
      e2e_game.player.spell_level[SPELL_FROST] == 0) && tries < 15) {
    int want = UPGRADE_SPELL_FIREBALL;
    int idx = 0;
    int guard = 0;
    e2e_clear_pools();
    e2e_game.player.xp = e2e_game.player.xp_next;
    e2e_frame(0);
    frames++;
    if (e2e_game.phase != PHASE_LEVELUP) return e2e_fail("levelup enter");
    if (!e2e_row_has(12, "LEVEL UP")) return e2e_fail("levelup text");
    if (e2e_game.player.spell_level[SPELL_FIREBALL] != 0) {
      want = UPGRADE_SPELL_FROST;
    }
    idx = e2e_find_choice(want);
    if (idx < 0) idx = 0;
    if (e2e_tap('1' + idx)) return e2e_fail("write choice");
    e2e_frame(0);
    frames++;
    while (e2e_game.phase == PHASE_LEVELUP && guard < 4) {
      if (e2e_tap('1')) return e2e_fail("write chain");
      e2e_frame(0);
      frames++;
      guard++;
    }
    if (e2e_game.phase != PHASE_PLAY) return e2e_fail("levelup exit");
    tries++;
  }
  if (e2e_game.player.spell_level[SPELL_FIREBALL] == 0) {
    return e2e_fail("acquire fireball");
  }
  if (e2e_game.player.spell_level[SPELL_FROST] == 0) {
    return e2e_fail("acquire frost");
  }
  if (e2e_game.player.level <= 1) return e2e_fail("level up");
  /* Fireball and frost casts. */
  e2e_clear_pools();
  e2e_plant(0, ENEMY_SLIME, 60, 0, 5000, 0);
  e2e_plant(1, ENEMY_SLIME, -60, 0, 5000, 0);
  i = 0;
  while (i < 20 && e2e_count_kind(SPELL_FIREBALL) < 1) {
    e2e_frame(0);
    frames++;
    i++;
  }
  if (e2e_count_kind(SPELL_FIREBALL) < 1) return e2e_fail("fireball cast");
  e2e_clear_pools();
  e2e_plant(0, ENEMY_SLIME, 40, 0, 5000, 0);
  i = 0;
  while (i < 20 && e2e_count_kind(SPELL_FROST) < 2) {
    e2e_frame(0);
    frames++;
    i++;
  }
  if (e2e_count_kind(SPELL_FROST) < 2) return e2e_fail("frost cast");
  /* Same-tick double kill: streak bonus on top of base scores. */
  {
    int k0 = 0;
    int s0 = 0;
    int s = 0;
    while (s < 5) {
      e2e_game.player.spell_level[s] = 0;
      s++;
    }
    e2e_game.player.spell_level[SPELL_FIREBALL] = 1;
    e2e_game.player.cooldown[SPELL_FIREBALL] = 1;
    e2e_clear_pools();
    e2e_plant(0, ENEMY_SLIME, 6, 0, 1, 0);
    e2e_plant(1, ENEMY_SLIME, 7, 0, 1, 0);
    k0 = e2e_game.kills;
    s0 = e2e_game.score;
    i = 0;
    while (i < 40 && e2e_game.kills < k0 + 2) {
      e2e_frame(0);
      frames++;
      i++;
    }
    if (e2e_game.kills < k0 + 2) return e2e_fail("streak kills");
    if (e2e_game.streak != 2) return e2e_fail("streak count");
    if (e2e_game.score - s0 < 25) return e2e_fail("streak score");
  }
  /* Pause freezes the tick and resumes through real keys. */
  if (e2e_tap('p')) return e2e_fail("write pause");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_PAUSE) return e2e_fail("pause enter");
  if (!e2e_row_has(20, "P A U S E D")) return e2e_fail("pause text");
  {
    int t = e2e_game.tick;
    e2e_frame(0);
    frames++;
    e2e_frame(0);
    frames++;
    e2e_frame(0);
    frames++;
    if (e2e_game.tick != t) return e2e_fail("pause freeze");
  }
  if (e2e_tap('p')) return e2e_fail("write resume");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_PLAY) return e2e_fail("pause exit");
  /* Loaded performance run: full spells, twenty foes, live spawns. */
  {
    int s = 0;
    int dir = 0;
    int code = 'd';
    int f = 0;
    while (s < 5) {
      e2e_game.player.spell_level[s] = 3;
      e2e_game.player.passive_level[s] = 0;
      s++;
    }
    e2e_clear_pools();
    i = 0;
    while (i < 20) {
      int kind = i % 6;
      int ang = i * 16;
      int dx = 0;
      int dy = 0;
      if (ang < 80) {
        dx = -80 + ang;
        dy = -60;
      } else if (ang < 160) {
        dx = 80;
        dy = -60 + (ang - 80);
      } else if (ang < 240) {
        dx = 80 - (ang - 160);
        dy = 60;
      } else {
        dx = -80;
        dy = 60 - (ang - 240);
      }
      e2e_plant(i, kind, dx, dy, 200, 5);
      e2e_game.enemies[i].speed = 128;
      i++;
    }
    e2e_game.player.invuln_timer = 100000;
    e2e_game.spawn_timer = 1;
    if (e2e_press('d')) return e2e_fail("write perf hold");
    while (f < 150) {
      unsigned a = 0;
      unsigned b = 0;
      int x = 0;
      int want = 0;
      if (f > 0 && f % 25 == 0) {
        if (e2e_release(code)) return e2e_fail("write perf swap");
        dir = !dir;
        code = dir ? 'a' : 'd';
        if (e2e_press(code)) return e2e_fail("write perf swap");
      }
      want = dir ? -512 : 512;
      x = e2e_game.player.x;
      a = get_current_jiffies();
      e2e_frame(&seen);
      b = get_current_jiffies();
      frames++;
      if (e2e_game.phase != PHASE_PLAY) return e2e_fail("perf play");
      if (e2e_game.player.x - x != want) return e2e_fail("perf move");
      if (seen.move_x != (dir ? -1 : 1)) return e2e_fail("perf input");
      j_total += b - a;
      if (b - a > j_max) j_max = b - a;
      if (f % 30 == 0) perf_hash[f / 30] = e2e_pixel_hash();
      if (f == 75) {
        if (e2e_dump("/shot_pixel.bin", get_pixel_fb(), E2E_PIXEL_WORDS)) {
          return e2e_fail("dump pixel");
        }
        if (e2e_dump("/shot_tilefb.bin", get_tile_fb(), E2E_TILEFB_WORDS)) {
          return e2e_fail("dump tilefb");
        }
        if (e2e_dump("/shot_tilemap.bin", get_tilemap(),
            E2E_TILEMAP_WORDS)) {
          return e2e_fail("dump tilemap");
        }
      }
      f++;
    }
    if (e2e_release(code)) return e2e_fail("write perf release");
    e2e_frame(0);
    frames++;
    i = 1;
    while (i < 5) {
      if (perf_hash[i] == perf_hash[i - 1]) return e2e_fail("perf live");
      i++;
    }
    if (j_max > 500) return e2e_fail("perf bound");
  }
  /* Transparent marker must never reach the pixel framebuffer. */
  e2e_clear_pools();
  e2e_game.player.invuln_timer = 0;
  e2e_frame(0);
  frames++;
  f000 = e2e_count_marker();
  if (f000 != 0) return e2e_fail("marker leak");
  faces = e2e_count_color(0xF9B);
  if (faces == 0) return e2e_fail("warden drawn");
  /* Lethal hit: game over, best score, and file round-trip. */
  e2e_clear_pools();
  {
    int s = 0;
    while (s < 5) {
      e2e_game.player.spell_level[s] = 0;
      s++;
    }
  }
  e2e_game.score = 900000;
  e2e_game.player.hp = 1;
  e2e_game.player.invuln_timer = 0;
  e2e_plant(0, ENEMY_GOLEM, 0, 0, 500, 25);
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_GAMEOVER) return e2e_fail("death");
  if ((e2e_game.sound & SOUND_GAMEOVER) == 0) return e2e_fail("over sound");
  if (e2e_game.best < 900000) return e2e_fail("best score");
  if (!e2e_row_has(8, "RIFT")) return e2e_fail("over text");
  {
    struct HighScores chk;
    save_load(&chk);
    if (chk.score[0] != e2e_game.best) return e2e_fail("best file");
    if (chk.time_sec[0] != game_seconds(&e2e_game)) {
      return e2e_fail("best time");
    }
    if (chk.kills[0] != e2e_game.kills) return e2e_fail("best kills");
    if (chk.level[0] != e2e_game.player.level) return e2e_fail("best level");
  }
  /* Restart through a real key. */
  if (e2e_tap('r')) return e2e_fail("write restart");
  e2e_frame(0);
  frames++;
  if (e2e_game.phase != PHASE_PLAY) return e2e_fail("restart");
  if (e2e_game.tick != 0) return e2e_fail("restart tick");
  if (e2e_game.player.hp != e2e_game.player.max_hp) {
    return e2e_fail("restart hp");
  }
  if (e2e_game.player.level != 1) return e2e_fail("restart level");
  /* Stats leave the guest on SD1, outside the one-line baseline. */
  {
    int fd = open("/e2e_stats.txt");
    if (fd < 0) return e2e_fail("stats open");
    write(fd, "frames ", 7);
    e2e_put_dec(fd, frames);
    write(fd, "\njiffies_total ", 14);
    e2e_put_dec(fd, j_total);
    write(fd, "\njiffies_max ", 12);
    e2e_put_dec(fd, j_max);
    write(fd, "\nhash_move ", 10);
    e2e_put_hex(fd, h0);
    write(fd, " ", 1);
    e2e_put_hex(fd, h1);
    write(fd, "\nhash_perf", 9);
    i = 0;
    while (i < 5) {
      write(fd, " ", 1);
      e2e_put_hex(fd, perf_hash[i]);
      i++;
    }
    write(fd, "\nscore ", 7);
    e2e_put_dec(fd, (unsigned)e2e_game.score);
    write(fd, " kills ", 7);
    e2e_put_dec(fd, (unsigned)e2e_game.kills);
    write(fd, " level ", 7);
    e2e_put_dec(fd, (unsigned)e2e_game.player.level);
    write(fd, "\nmarker_leaks ", 13);
    e2e_put_dec(fd, (unsigned)f000);
    write(fd, " face_pixels ", 13);
    e2e_put_dec(fd, (unsigned)faces);
    write(fd, "\nbest ", 6);
    e2e_put_dec(fd, (unsigned)e2e_game.best);
    write(fd, "\n", 1);
    close(fd);
  }
  puts("***PASS emberfall e2e\n");
  return 0;
}
