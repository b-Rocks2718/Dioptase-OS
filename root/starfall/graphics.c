#include "graphics.h"

#include "../crt/sys.h"
#include "../crt/vga.h"
#include "../crt/print.h"

/*
 * User-mode mappings cover physical 0x7FBD000..0x7FBF57F (80x60 tile entries),
 * 0x7FE8000..0x7FEFFFF (256 tiles of 8x8 16-bit pixels), and
 * 0x7FF0000..0x7FF7FFF (16 sprites of 32x32 16-bit pixels). Sprite 0 and 1
 * coordinates use the syscall-backed 0x7FE5B00..0x7FE5B3F registers; their
 * scale registers are in 0x7FE5B60..0x7FE5B6F. Hidden sprites use 0xF000
 * transparent pixels, whose effect is specified for sprite data. The process holds borrowed
 * mappings; the terminal is given eight VGA frames to
 * hide its cursor and clear before this foreground program writes them. Tile
 * writes change display memory directly; precise scanout visibility and
 * atomicity relative to a frame are unspecified by docs/mem_map.md. The game
 * observes the 60 Hz frame counter before each update and writes changed cells
 * in order. The shell restores the terminal display after the process exits.
 */
#define FIELD_LEFT 2
#define FIELD_TOP 10
#define SIDE_LEFT 57
#define COLOR_WHITE 0xFF
#define COLOR_CYAN 0x5F
#define COLOR_PINK 0xD7
#define COLOR_AMBER 0xFC
#define COLOR_DIM 0x6D
#define COLOR_RED 0xE0

#define ART_VOID 128
#define ART_STAR 129
#define ART_STAR_BRIGHT 130
#define ART_ROCK 131
#define ART_SHIP 132
#define ART_HUNTER 133
#define ART_SNIPER 134
#define ART_BRUTE 135
#define ART_CORE_FIRST 136
#define ART_PLAYER_SHOT 145
#define ART_ENEMY_SHOT 146
#define ART_ENERGY 147
#define ART_PULSE 148
#define PLAYER_SPRITE 0
#define CORE_SPRITE 1
#define SPRITE_PIXELS 32
#define SPRITE_CELL_PIXELS 1024
#define OWNED_SPRITE_PIXELS 2048
#define SPRITE_ANCHOR_OFFSET 12

static volatile short* tilemap;
static volatile short* framebuffer;
static volatile short* spritemap;
static short sprite_art[OWNED_SPRITE_PIXELS];
static int sprite_visible[2];
static short wanted[FIELD_CELLS];
static short displayed[FIELD_CELLS];
static int previous_phase = -1;
static int previous_paused = -1;

/* The small signed offsets used for the pulse ring have bounded magnitude. */
static int magnitude(int value) {
  return value < 0 ? -value : value;
}

/* Paint an on-screen character without writing beyond the 80x60 tile array. */
static void put_char(int x, int y, int character, int color) {
  if (x >= 0 && y >= 0 && x < TILE_ROW_WIDTH && y < TILE_COL_HEIGHT) {
    framebuffer[y * TILE_ROW_WIDTH + x] =
      (short)((color << 8) | (character & 255));
  }
}

/* Write an ASCII label, with the VGA glyphs retaining their installed shapes. */
static void put_text(int x, int y, char* text, int color) {
  for (int i = 0; text[i] != '\0'; ++i) {
    put_char(x + i, y, text[i], color);
  }
}

/* Display small nonnegative counters without depending on libc formatting. */
static void put_number(int x, int y, unsigned value, int color) {
  char digits[11];
  int count = 0;
  do {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0 && count < 10);
  for (int i = count - 1; i >= 0; --i) {
    put_char(x++, y, digits[i], color);
  }
}

/* Replace a rectangular text region, including old numbers of different widths. */
static void clear_rect(int x, int y, int width, int height) {
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      put_char(x + col, y + row, ' ', COLOR_WHITE);
    }
  }
}

/* Give intermission and end screens a solid readable interior. */
static void draw_box(int x, int y, int width, int height, int color) {
  clear_rect(x, y, width, height);
  for (int col = 0; col < width; ++col) {
    put_char(x + col, y, col == 0 || col == width - 1 ? '+' : '-', color);
    put_char(x + col, y + height - 1,
             col == 0 || col == width - 1 ? '+' : '-', color);
  }
  for (int row = 1; row < height - 1; ++row) {
    put_char(x, y + row, '|', color);
    put_char(x + width - 1, y + row, '|', color);
  }
}

/* The tile art uses a compact readable palette; 0x0BGR is the device format. */
static short art_color(char pixel) {
  if (pixel == ' ') return 0x102;
  if (pixel == '.') return 0x213;
  if (pixel == 's') return 0xBDD;
  if (pixel == 'w') return 0xFFF;
  if (pixel == 'c') return 0xFA2;
  if (pixel == 'C') return 0xFF8;
  if (pixel == 'b') return 0xB51;
  if (pixel == 'm') return 0xA3F;
  if (pixel == 'M') return 0xD8F;
  if (pixel == 'r') return 0x537;
  if (pixel == 'R') return 0x324;
  if (pixel == 'o') return 0x17F;
  if (pixel == 'y') return 0x5FF;
  if (pixel == 'g') return 0x6F5;
  return 0x102;
}

/* Copy an 8x8 text drawing into one documented 64-pixel tile slot. */
static void paint_art(int tile, char** rows) {
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      tilemap[tile * 64 + y * 8 + x] = art_color(rows[y][x]);
    }
  }
}

/* Build the custom ship, threats, terrain, bolts, and pickups after text glyphs. */
static void install_art(void) {
  char* void_art[8] = {
    "        ", "        ", "        ", "        ",
    "        ", "        ", "        ", "        "
  };
  char* star_art[8] = {
    "        ", "        ", "        ", "   .    ",
    "        ", "        ", "        ", "        "
  };
  char* bright_art[8] = {
    "        ", "        ", "   .    ", "  .s.   ",
    "   .    ", "        ", "        ", "        "
  };
  char* rock_art[8] = {
    "  rRRr  ", " rRRRRRr ", "rRRrrRRr", "RRRrrRRR",
    "RRRrrRRR", "rRRRRRRr", " rRRRRr ", "  rrrr  "
  };
  char* ship_art[8] = {
    "   c    ", "  cCc   ", "  cCCc  ", " ccCCcc ",
    "cCyyyyCc", " ccMMcc ", "  M  M  ", "        "
  };
  char* hunter_art[8] = {
    "  m  m  ", " mMMMMm ", "mMMmmMMm", "MMmwwmMM",
    "MMmwwmMM", "mMMmmMMm", " mMMMMm ", "  m  m  "
  };
  char* sniper_art[8] = {
    "   o    ", "  ooo   ", " ooyoo  ", "oooyyooo",
    "oooyyooo", " ooyoo  ", "  ooo   ", "   o    "
  };
  char* brute_art[8] = {
    " rrrrrr ", "rRrrrrRr", "rRooooRr", "rrOwwOrr",
    "rrOwwOrr", "rRooooRr", "rRrrrrRr", " rrrrrr "
  };
  char* bolt_art[8] = {
    "        ", "        ", "   yy   ", "  ywwy  ",
    "  ywwy  ", "   yy   ", "        ", "        "
  };
  char* hostile_bolt_art[8] = {
    "        ", "   oo   ", "  oooo  ", " ooMMoo ",
    " ooMMoo ", "  oooo  ", "   oo   ", "        "
  };
  char* energy_art[8] = {
    "        ", "   g    ", "  gCg   ", " gCwCg  ",
    "  gCg   ", "   g    ", "        ", "        "
  };
  char* pulse_art[8] = {
    "c      c", " c    c ", "  c  c  ", "   CC   ",
    "   CC   ", "  c  c  ", " c    c ", "c      c"
  };

  paint_art(ART_VOID, void_art);
  paint_art(ART_STAR, star_art);
  paint_art(ART_STAR_BRIGHT, bright_art);
  paint_art(ART_ROCK, rock_art);
  paint_art(ART_SHIP, ship_art);
  paint_art(ART_HUNTER, hunter_art);
  paint_art(ART_SNIPER, sniper_art);
  paint_art(ART_BRUTE, brute_art);
  paint_art(ART_PLAYER_SHOT, bolt_art);
  paint_art(ART_ENEMY_SHOT, hostile_bolt_art);
  paint_art(ART_ENERGY, energy_art);
  paint_art(ART_PULSE, pulse_art);

  /* The 24x24 core is split across nine adjacent tile slots. */
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 24; ++x) {
      int dx = x - 11;
      int dy = y - 11;
      int distance = dx * dx + dy * dy;
      char pixel = ' ';
      if (distance < 130) pixel = 'm';
      if (distance < 104) pixel = 'r';
      if (distance < 70) pixel = 'M';
      if (distance < 35) pixel = 'o';
      if (distance < 16) pixel = 'w';
      if ((x == 11 || x == 12 || y == 11 || y == 12) && distance < 130) {
        pixel = distance < 35 ? 'w' : 'c';
      }
      int tile = ART_CORE_FIRST + (y / 8) * 3 + x / 8;
      tilemap[tile * 64 + (y % 8) * 8 + x % 8] = art_color(pixel);
    }
  }
}

/* Paint two 32x32 transparent sprites: an interceptor and the Breaker core. */
static void install_sprites(void) {
  for (int y = 0; y < SPRITE_PIXELS; ++y) {
    for (int x = 0; x < SPRITE_PIXELS; ++x) {
      int dx = x - 15;
      int ax = magnitude(dx);
      short ship = 0xF000;
      int hull_width = y > 2 ? (y - 2) / 3 + 1 : 0;
      if (y >= 3 && y <= 25 && ax <= hull_width) {
        ship = ax == hull_width ? 0xFA2 : 0xB51;
      }
      if (y >= 14 && y <= 25 && ax >= 5 && ax <= 14 - (y - 14) / 3) {
        ship = ax >= 12 - (y - 14) / 3 ? 0xFA2 : 0x638;
      }
      if (y >= 7 && y <= 16 && ax <= 2) ship = 0xFFE;
      if (y >= 24 && y <= 29 && ax <= 3) ship = y <= 26 ? 0x5FF : 0x17F;
      if (y >= 28 && y <= 31 && ax <= 1) ship = 0xFFF;
      sprite_art[PLAYER_SPRITE * SPRITE_CELL_PIXELS +
                 y * SPRITE_PIXELS + x] = ship;

      int core_dx = x - 15;
      int core_dy = y - 15;
      int radius_sq = core_dx * core_dx + core_dy * core_dy;
      short core = 0xF000;
      if (radius_sq < 235) core = 0xA3F;
      if (radius_sq < 190) core = 0x537;
      if (radius_sq < 145) core = 0xD8F;
      if (radius_sq < 92) core = 0x17F;
      if (radius_sq < 53) core = 0x00F;
      if (radius_sq < 19) core = 0xFFF;
      if ((ax <= 1 || magnitude(core_dy) <= 1) && radius_sq < 235) {
        core = radius_sq < 53 ? 0xFFF : 0xFA2;
      }
      sprite_art[CORE_SPRITE * SPRITE_CELL_PIXELS +
                 y * SPRITE_PIXELS + x] = core;
    }
  }
}

/* Publish opaque art or documented transparent pixels only when visibility changes. */
static void show_sprite(int slot, int visible) {
  if (sprite_visible[slot] == visible) return;
  for (int pixel = 0; pixel < SPRITE_CELL_PIXELS; ++pixel) {
    spritemap[slot * SPRITE_CELL_PIXELS + pixel] =
      visible ? sprite_art[slot * SPRITE_CELL_PIXELS + pixel] : 0xF000;
  }
  sprite_visible[slot] = visible;
}

/* Lay out the fixed title, arena frame, side divider, and controls. */
static void draw_chrome(void) {
  clear_rect(0, 0, TILE_ROW_WIDTH, TILE_COL_HEIGHT);
  put_text(2, 1, "S T A R F A L L", COLOR_CYAN);
  put_text(2, 2, "THE LAST SIGNAL", COLOR_PINK);
  put_text(2, 4, "A neon survival arcade in the debris of a dying star", COLOR_DIM);
  for (int x = 1; x < 55; ++x) {
    put_char(x, 9, '=', COLOR_CYAN);
    put_char(x, 48, '=', COLOR_CYAN);
  }
  for (int y = 10; y < 48; ++y) {
    put_char(1, y, '|', COLOR_CYAN);
    put_char(54, y, '|', COLOR_CYAN);
    put_char(56, y, '|', COLOR_DIM);
  }
  put_text(2, 50, "W A S D  FLY    SPACE  NOVA    P  PAUSE    Q  QUIT", COLOR_WHITE);
  put_text(2, 52, "Your blaster tracks the nearest threat automatically.", COLOR_DIM);
  put_text(2, 55, "Collect green energy. Survive four waves. Break the CORE.", COLOR_AMBER);
  put_text(2, 58, "One last transmission from the edge of the universe.", COLOR_DIM);
}

/* Clear and redraw the live counters and upgrade levels in the right panel. */
static void draw_status(struct Game* game) {
  clear_rect(SIDE_LEFT, 10, 23, 38);
  put_text(SIDE_LEFT + 1, 11, "MISSION STATUS", COLOR_CYAN);
  if (game->phase == PHASE_TITLE) {
    put_text(SIDE_LEFT + 1, 16, "AWAITING PILOT", COLOR_PINK);
    put_text(SIDE_LEFT + 1, 21, "FIVE SECTORS", COLOR_DIM);
    put_text(SIDE_LEFT + 1, 23, "ONE LAST STAR", COLOR_DIM);
    put_text(SIDE_LEFT + 1, 29, "RECORD", COLOR_DIM);
    put_number(SIDE_LEFT + 1, 31, game->best, COLOR_AMBER);
    return;
  }
  put_text(SIDE_LEFT + 1, 13, "WAVE", COLOR_DIM);
  put_number(SIDE_LEFT + 8, 13, (unsigned)game->wave, COLOR_WHITE);
  put_char(SIDE_LEFT + 9, 13, '/', COLOR_WHITE);
  put_number(SIDE_LEFT + 10, 13, FINAL_WAVE, COLOR_WHITE);
  put_text(SIDE_LEFT + 1, 16, "HULL", COLOR_DIM);
  put_number(SIDE_LEFT + 8, 16, (unsigned)game->hp, COLOR_PINK);
  put_char(SIDE_LEFT + 9, 16, '/', COLOR_DIM);
  put_number(SIDE_LEFT + 10, 16, (unsigned)game->max_hp, COLOR_WHITE);
  put_text(SIDE_LEFT + 1, 19, "NOVA", COLOR_DIM);
  for (int i = 0; i < 10; ++i) {
    put_char(SIDE_LEFT + 1 + i, 21,
             i * (PULSE_FULL / 10) < game->pulse_charge ? '#' : '.',
             game->pulse_charge >= PULSE_FULL ? COLOR_CYAN : COLOR_DIM);
  }
  put_text(SIDE_LEFT + 1, 24, "SCORE", COLOR_DIM);
  put_number(SIDE_LEFT + 1, 26, game->score, COLOR_AMBER);
  put_text(SIDE_LEFT + 1, 28, "BEST", COLOR_DIM);
  put_number(SIDE_LEFT + 1, 30, game->best, COLOR_AMBER);
  put_text(SIDE_LEFT + 1, 33, "CONTACTS", COLOR_DIM);
  put_number(SIDE_LEFT + 1, 35, (unsigned)game_enemy_count(game), COLOR_RED);
  put_text(SIDE_LEFT + 1, 38, "TECH", COLOR_CYAN);
  put_text(SIDE_LEFT + 1, 40, "GUN", COLOR_DIM);
  put_number(SIDE_LEFT + 7, 40, (unsigned)game->rapid_level, COLOR_WHITE);
  put_text(SIDE_LEFT + 1, 42, "WING", COLOR_DIM);
  put_number(SIDE_LEFT + 7, 42, (unsigned)game->twin_level, COLOR_WHITE);
  put_text(SIDE_LEFT + 1, 44, "NOVA", COLOR_DIM);
  put_number(SIDE_LEFT + 7, 44, (unsigned)game->emp_level, COLOR_WHITE);
}

/* Keep the top broadcast strip aligned with the current sector and phase. */
static void draw_broadcast(struct Game* game, bool paused) {
  clear_rect(2, 6, 53, 2);
  if (paused) {
    put_text(2, 7, "== TRANSMISSION HOLD ==", COLOR_AMBER);
  } else if (game->phase == PHASE_TITLE) {
    put_text(2, 7, "CHANNEL OPEN  /  PILOT NOT DETECTED", COLOR_DIM);
  } else if (game->phase == PHASE_UPGRADE) {
    put_text(2, 7, "SECTOR CLEAR  /  SALVAGE LINK ESTABLISHED", COLOR_CYAN);
  } else if (game->phase == PHASE_DEAD) {
    put_text(2, 7, "SIGNAL LOST  /  SEARCHING FOR SURVIVORS", COLOR_PINK);
  } else if (game->phase == PHASE_VICTORY) {
    put_text(2, 7, "THE BREAKER FLEET HAS FALLEN SILENT", COLOR_AMBER);
  } else if (game->wave == FINAL_WAVE) {
    put_text(2, 7, "WARNING  /  BREAKER CORE SIGNATURE DETECTED",
             game->frame % 20 < 10 ? COLOR_AMBER : COLOR_PINK);
  } else {
    put_text(2, 7, "INCOMING WAVE", COLOR_AMBER);
    put_number(17, 7, (unsigned)game->wave, COLOR_WHITE);
    put_text(20, 7, "/  SURVIVE AND COLLECT ENERGY", COLOR_DIM);
  }
}

/* Overlay one menu on the quiet arena between active waves or after a run. */
static void draw_overlay(struct Game* game, bool paused) {
  if (paused && game->phase == PHASE_PLAY) {
    draw_box(11, 20, 36, 17, COLOR_CYAN);
    put_text(23, 24, "PAUSED", COLOR_AMBER);
    put_text(16, 29, "P resume    Q quit", COLOR_WHITE);
    put_text(16, 33, "Breathe. The stars can wait.", COLOR_DIM);
  } else if (game->phase == PHASE_TITLE) {
    draw_box(7, 15, 43, 27, COLOR_CYAN);
    put_text(12, 18, "*** INCOMING DISTRESS CALL ***", COLOR_AMBER);
    put_text(12, 22, "The Breaker fleet found the last", COLOR_WHITE);
    put_text(12, 24, "light in this sector. Hold it.", COLOR_WHITE);
    put_text(12, 28, "WASD  maneuver your interceptor", COLOR_CYAN);
    put_text(12, 30, "SPACE unleash a charged NOVA", COLOR_PINK);
    put_text(12, 32, "1/2/3 select salvage upgrades", COLOR_AMBER);
    put_text(12, 36, "PRESS ENTER TO LAUNCH", COLOR_WHITE);
    put_text(12, 39, "P pause   Q return to shell", COLOR_DIM);
  } else if (game->phase == PHASE_UPGRADE) {
    draw_box(6, 16, 45, 26, COLOR_CYAN);
    put_text(11, 19, "SECTOR CLEAR  -  CHOOSE SALVAGE", COLOR_AMBER);
    for (int option = 0; option < 3; ++option) {
      int row = 24 + option * 5;
      put_char(11, row, '1' + option, COLOR_PINK);
      put_text(14, row, game_upgrade_name(game->upgrade_choices[option]),
               COLOR_WHITE);
      put_text(14, row + 2, game_upgrade_effect(game->upgrade_choices[option]),
               COLOR_DIM);
    }
    put_text(11, 39, "Press 1, 2 or 3 to continue", COLOR_CYAN);
  } else if (game->phase == PHASE_DEAD) {
    draw_box(8, 18, 41, 20, COLOR_PINK);
    put_text(15, 22, "SIGNAL LOST", COLOR_PINK);
    put_text(14, 27, "The stars remember your stand.", COLOR_WHITE);
    put_text(14, 31, "ENTER retry    Q quit", COLOR_CYAN);
  } else if (game->phase == PHASE_VICTORY) {
    draw_box(7, 16, 43, 24, COLOR_CYAN);
    put_text(13, 20, "THE CORE IS SILENT", COLOR_AMBER);
    put_text(12, 25, "One last signal crosses the dark.", COLOR_WHITE);
    put_text(12, 29, "The sector is yours, pilot.", COLOR_CYAN);
    put_text(12, 35, "ENTER fly again    Q quit", COLOR_PINK);
  }
}

/* Choose the final picture for an arena cell, in increasing depth order. */
static void compose_arena(struct Game* game) {
  for (int index = 0; index < FIELD_CELLS; ++index) {
    int art = game->cells[index] == CELL_ROCK ? ART_ROCK :
              game->stars[index] == 1 &&
              ((game->frame / 16 + (unsigned)index) % 9 == 0)
                ? ART_STAR_BRIGHT :
              game->stars[index] == 1 ? ART_STAR :
              game->stars[index] == 2 ? ART_STAR_BRIGHT : ART_VOID;
    wanted[index] = (short)art;
  }
  for (int i = 0; i < MAX_DROPS; ++i) {
    if (game->drops[i].active) {
      int index = game->drops[i].y * FIELD_W + game->drops[i].x;
      wanted[index] = ART_ENERGY;
    }
  }
  for (int i = 0; i < MAX_SHOTS; ++i) {
    if (game->shots[i].active) {
      int x = game->shots[i].fx >> 8;
      int y = game->shots[i].fy >> 8;
      if (x >= 0 && y >= 0 && x < FIELD_W && y < FIELD_H) {
        wanted[y * FIELD_W + x] = game->shots[i].side == SHOT_PLAYER
                                    ? ART_PLAYER_SHOT : ART_ENEMY_SHOT;
      }
    }
  }
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    struct Enemy* enemy = &game->enemies[i];
    if (!enemy->active) continue;
    if (enemy->kind == ENEMY_CORE) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          int x = enemy->x + dx;
          int y = enemy->y + dy;
          if (x >= 0 && y >= 0 && x < FIELD_W && y < FIELD_H) {
            wanted[y * FIELD_W + x] =
              (short)(ART_CORE_FIRST + (dy + 1) * 3 + dx + 1);
          }
        }
      }
    } else {
      wanted[enemy->y * FIELD_W + enemy->x] =
        (short)(enemy->kind == ENEMY_HUNTER ? ART_HUNTER :
                enemy->kind == ENEMY_SNIPER ? ART_SNIPER : ART_BRUTE);
    }
  }
  if (game->pulse_flash > 0) {
    int radius = 12 - game->pulse_flash;
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (magnitude(dx) + magnitude(dy) == radius) {
          int x = game->player_x + dx;
          int y = game->player_y + dy;
          if (x >= 0 && y >= 0 && x < FIELD_W && y < FIELD_H) {
            wanted[y * FIELD_W + x] = ART_PULSE;
          }
        }
      }
    }
  }
  if (game->phase == PHASE_PLAY) {
    wanted[game->player_y * FIELD_W + game->player_x] =
      game->hurt_timer > 0 && (game->frame / 3) % 2 == 0
        ? ART_PULSE : ART_SHIP;
  }
}

/* Position only the two owned sprites, hiding them when menus cover the field. */
static void position_sprites(struct Game* game, bool paused) {
  if (game->phase == PHASE_PLAY && !paused) {
    show_sprite(PLAYER_SPRITE, 1);
    set_sprite_coords(PLAYER_SPRITE,
                      (FIELD_LEFT + game->player_x) * TILE_HEIGHT -
                        SPRITE_ANCHOR_OFFSET,
                      (FIELD_TOP + game->player_y) * TILE_HEIGHT -
                        SPRITE_ANCHOR_OFFSET);
  } else show_sprite(PLAYER_SPRITE, 0);
  for (int i = 0; i < MAX_ENEMIES; ++i) {
    if (game->phase == PHASE_PLAY && !paused &&
        game->enemies[i].active && game->enemies[i].kind == ENEMY_CORE) {
      show_sprite(CORE_SPRITE, 1);
      set_sprite_coords(CORE_SPRITE,
                        (FIELD_LEFT + game->enemies[i].x) * TILE_HEIGHT -
                          SPRITE_ANCHOR_OFFSET,
                        (FIELD_TOP + game->enemies[i].y) * TILE_HEIGHT -
                          SPRITE_ANCHOR_OFFSET);
      return;
    }
  }
  show_sprite(CORE_SPRITE, 0);
}

/* Claim the VGA tile mappings and initialize every pixel we will display. */
bool graphics_init(void) {
  framebuffer = get_tile_fb();
  tilemap = get_tilemap();
  spritemap = get_spritemap();
  if (framebuffer == (volatile short*)-1) {
    puts("starfall VGA: get_tile_fb could not map 0x7FBD000..0x7FBF57F\n");
    return false;
  }
  if (tilemap == (volatile short*)-1) {
    puts("starfall VGA: get_tilemap could not map 0x7FE8000..0x7FEFFFF\n");
    return false;
  }
  if (spritemap == (volatile short*)-1) {
    puts("starfall VGA: get_spritemap could not map 0x7FF0000..0x7FF7FFF\n");
    return false;
  }
  load_text_tiles();
  install_art();
  install_sprites();
  for (int slot = 0; slot < 2; ++slot) {
    for (int pixel = 0; pixel < SPRITE_CELL_PIXELS; ++pixel) {
      spritemap[slot * SPRITE_CELL_PIXELS + pixel] = 0xF000;
    }
    sprite_visible[slot] = 0;
  }
  clear_screen();
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  set_sprite_scale(PLAYER_SPRITE, 0);
  set_sprite_scale(CORE_SPRITE, 0);
  draw_chrome();
  for (int i = 0; i < FIELD_CELLS; ++i) displayed[i] = -1;
  previous_phase = -1;
  previous_paused = -1;
  return true;
}

/* Update changed cells; phase transitions repaint cells hidden by menus. */
void graphics_draw(struct Game* game, bool paused) {
  int phase_changed = game->phase != previous_phase ||
                      (int)paused != previous_paused;
  if (phase_changed) {
    for (int i = 0; i < FIELD_CELLS; ++i) displayed[i] = -1;
    previous_phase = game->phase;
    previous_paused = paused;
  }
  compose_arena(game);
  for (int index = 0; index < FIELD_CELLS; ++index) {
    if (wanted[index] != displayed[index]) {
      int x = index % FIELD_W;
      int y = index / FIELD_W;
      put_char(FIELD_LEFT + x, FIELD_TOP + y, wanted[index] & 255, 0);
      displayed[index] = wanted[index];
    }
  }
  if (phase_changed || game->frame % 10 == 0) {
    draw_status(game);
    draw_broadcast(game, paused);
  }
  if (phase_changed) draw_overlay(game, paused);
  position_sprites(game, paused);
}
