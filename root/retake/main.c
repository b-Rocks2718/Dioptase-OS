#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"
#include "retake.h"
#include "game.h"
#include "pak.h"

/*
 * RETAKE: a stunt double, a cursed film studio, and the takes she records.
 * A game by Claude Opus and GPT-6 Astra, for Dioptase-OS.
 *
 * main() owns the display and the frame loop: the title screen, then fixed
 * 60 Hz game ticks (game.c) paced by the VGA frame counter, with one
 * composited frame presented per loop. Scripts the game asks for (lore,
 * signs, deaths) are shown as intertitle cards between ticks.
 */

/* Hero animation frames, in tools/sprites.py HERO_ORDER. */
#define FR_IDLE0 0
#define FR_IDLE1 1
#define FR_RUN0 2
#define FR_RUN1 3
#define FR_RUN2 4
#define FR_JUMP 5
#define FR_FALL 6
#define FR_CLAP0 7
#define FR_CLAP1 8
#define FR_WALL 9
#define FR_HURT 10
#define FR_GLIDE 11

#define SLOT_HERO 0
#define SLOT_TAKE0 1

/* Where the 8x12 body's top-left sits on the 32x32 sprite canvas
 * (tools/sprites.py: CENTER_X 16, FEET_Y 27). */
#define CANVAS_BODY_X 12
#define CANVAS_BODY_Y 15

#define START_ROOM ROOM_BACKLOT_WEST

static int cam_x;
static int cam_y;
static unsigned frame_clock;
static int current_song = -1;
static int current_region = -1;

static short title_ramp[4] = {0, 0x345, 0xABC, 0xEFF};
static short dim_ramp[4] = {0, 0x123, 0x567, 0x89A};
static short red_ramp[4] = {0, 0x113, 0x238, 0x34F};

/* Animation frame for a pose; running strides advance with distance. */
static int frame_for(int pose, int clapping_active, int x, int clock) {
  switch (pose) {
    case POSE_RUN: {
      int phase = (x >> 11) & 3;
      if (phase == 0) return FR_RUN0;
      if (phase == 2) return FR_RUN2;
      return FR_RUN1;
    }
    case POSE_RISE: return FR_JUMP;
    case POSE_FALL: return FR_FALL;
    case POSE_WALL: return FR_WALL;
    case POSE_GLIDE: return FR_GLIDE;
    case POSE_CLAP: return clapping_active ? FR_CLAP1 : FR_CLAP0;
    case POSE_HURT: return FR_HURT;
    default: return ((clock >> 5) & 1) ? FR_IDLE1 : FR_IDLE0;
  }
}

/* Ease the camera toward Ruby (leading a little where she faces) and keep it
 * inside the room. The visible play area is pixel rows 8..235. */
static void update_camera(int snap) {
  int hx = (g_hero.x >> 8) + (PLAYER_W >> 1);
  int hy = (g_hero.y >> 8) + (PLAYER_H >> 1);
  int tx = hx - (SCREEN_W >> 1) + (g_hero.facing << 4);
  int ty = hy - 122;
  if (snap) {
    cam_x = tx;
    cam_y = ty;
  } else {
    cam_x += (tx - cam_x) >> 3;
    cam_y += (ty - cam_y) >> 3;
  }
  int max_x = g_room_pw - SCREEN_W;
  int max_y = g_room_ph - VIEW_H;
  if (cam_x > max_x) cam_x = max_x;
  if (cam_y > max_y) cam_y = max_y;
  if (cam_x < 0) cam_x = 0;
  if (cam_y < -HUD_PIXEL_ROWS) cam_y = -HUD_PIXEL_ROWS;
  if (cam_y < 0) cam_y = 0;
}

/* Region 2 dresses the same behaviors as Western characters. */
static int region2_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER2_0 + ((e->x >> 11) & 1);
    case ENT_HOPPER: return IMG_HOPPER2_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER2_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER2_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER2_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return e->b ? IMG_COAL_0 : IMG_SHOT2_0;
    default: return -1;
  }
}

/* The image for entity `e` this frame, or -1 to skip drawing. */
static int ent_image(struct Ent* e) {
  if (current_region == 2) {
    int v = region2_image(e);
    if (v >= 0) return v;
  }
  switch (e->kind) {
    case ENT_COIN: return IMG_COIN_0 + ((e->anim >> 3) & 3);
    case ENT_HEART: return IMG_HEART_0 + ((frame_clock >> 4) & 1);
    case ENT_FRAME: return IMG_FRAME_0;
    case ENT_REEL: return IMG_REEL_0 + ((frame_clock >> 3) & 1);
    case ENT_ABILITY: return IMG_ABILITY_0;
    case ENT_MIRROR: return IMG_MIRROR_0;
    case ENT_SIGN: return IMG_SIGN_0;
    case ENT_NPC: return IMG_NPC_0;
    case ENT_PLATE: return IMG_PLATE_0 + (e->state ? 1 : 0);
    case ENT_DOOR: return e->d ? -1 : IMG_DOOR_0;   /* d: the room art already shows the door */
    case ENT_WALKER: return IMG_WALKER_0 + ((e->x >> 11) & 1);
    case ENT_HOPPER: return IMG_HOPPER_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return IMG_SHOT_0;
    case ENT_SANDBAG: return IMG_SANDBAG_0;
    case ENT_SPRING: return IMG_SPRING_0 + (e->timer > 0 ? 1 : 0);
    case ENT_SHOP: return IMG_SHOP_0;
    case ENT_WAVE: return IMG_WAVE_0;
    case ENT_PLATFORM: return IMG_CART_0;
    case ENT_BOSS:
      if (e->a == 2) return e->state == 13 ? IMG_BOSS2_3 : IMG_BOSS2_0 + ((e->x >> 10) % 3 + 3) % 3;
      switch (e->state) {
        case 2: return IMG_BOSS1_2;            /* slam wind-up: baton raised */
        case 3: return IMG_BOSS1_3;            /* slam */
        case 4: case 5: return IMG_BOSS1_4;    /* charge */
        case 6: return IMG_BOSS1_5;            /* dazed */
        case 7: return IMG_BOSS1_6;            /* whistle */
        default: return IMG_BOSS1_0 + ((frame_clock >> 4) & 1);
      }
    default: return -1;
  }
}

static int interactable(int kind) {
  return kind == ENT_MIRROR || kind == ENT_SIGN || kind == ENT_NPC || kind == ENT_DOOR || kind == ENT_SHOP;
}

static void draw_entities(void) {
  struct Hitbox hb;
  hb.x = g_hero.x;
  hb.y = g_hero.y;
  hb.w = PLAYER_W * Q8;
  hb.h = PLAYER_H * Q8;
  for (int i = 0; i < g_ent_count; i++) {
    struct Ent* e = &g_ents[i];
    if (!e->alive) continue;
    if (e->hurt && (frame_clock & 2)) continue;   /* hit flash */
    int ex = (e->x >> 8) - cam_x;
    int ey = (e->y >> 8) - cam_y;
    int ew = e->w >> 8;
    int eh = e->h >> 8;
    if (e->kind == ENT_GATE) {
      if (e->state) continue;                     /* open: slid out of view */
      int h = e->b > 0 ? e->b : 1;
      for (int k = 0; k < h; k++) draw_image(IMG_GATE_0, ex, ey + (k << 3), 0);
      continue;
    }
    int img = ent_image(e);
    if (img < 0) continue;
    int iw = image_width(img);
    int ih = image_height(img);
    int flip = e->dir > 0 && (e->kind == ENT_WALKER || e->kind == ENT_FLYER || e->kind == ENT_THROWER ||
                              e->kind == ENT_CHARGER || e->kind == ENT_HOPPER || e->kind == ENT_BOSS);
    int x = ex + (ew >> 1) - (iw >> 1);
    int y = ey + eh - ih;
    if (e->kind == ENT_PLATFORM) y = ey;
    if (e->kind == ENT_SANDBAG || e->kind == ENT_SHOT || e->kind == ENT_FLYER || e->kind == ENT_COIN) y = ey;
    draw_image(img, x, y, flip);
    if (interactable(e->kind)) {
      struct Hitbox eb;
      eb.x = e->x;
      eb.y = e->y;
      eb.w = e->w;
      eb.h = e->h;
      if (hitbox_overlap(&hb, &eb)) draw_prompt(ex + (ew >> 1), y - 8, (int)frame_clock);
    }
  }
}

/* The boss's remaining health as a bar along the bottom of the play area. */
static void draw_boss_bar(void) {
  if (!g_boss || !g_boss->alive) return;
  int total = g_boss->a == 2 ? 10 : 16;
  int hp = g_boss->hp < 0 ? 0 : g_boss->hp;
  int x0 = 80;
  int w = 160;
  int filled = (w * hp) / total;
  for (int y = 226; y < 232; y++) {
    unsigned short* row = g_back + y * SCREEN_W;
    for (int x = x0 - 1; x <= x0 + w; x++) {
      int edge = y == 226 || y == 231 || x == x0 - 1 || x == x0 + w;
      if (edge) row[x] = 0x0111;
      else row[x] = (unsigned short)((x - x0) < filled ? 0x022E : 0x0213);
    }
  }
}

static void draw_sprites(void) {
  struct Hitbox box;
  int active = player_attack_box(&g_hero, &box);
  int f = frame_for(g_hero.pose, active, g_hero.x, (int)frame_clock);
  if (g_hero.hurt_timer > 0 && (frame_clock & 4)) {
    sprite_hide(SLOT_HERO);
  } else {
    sprite_show(SLOT_HERO, SHEET_HERO, f, g_hero.facing,
                (g_hero.x >> 8) - cam_x - CANVAS_BODY_X, (g_hero.y >> 8) - cam_y - CANVAS_BODY_Y);
  }
  int sheet = ((frame_clock >> 1) & 1) ? SHEET_GHOST_B : SHEET_GHOST_A;
  for (int i = 0; i < TAKE_MAX_SLOTS; i++) {
    struct Take* t = &g_takes.slots[i];
    if (!t->active || t->length == 0) {
      sprite_hide(SLOT_TAKE0 + i);
      continue;
    }
    struct TakeFrame* fr = &t->frames[t->cursor];
    int tf = frame_for(fr->pose, fr->attack_dir != 0, fr->x, (int)frame_clock);
    sprite_show(SLOT_TAKE0 + i, sheet, tf, fr->facing,
                (fr->x >> 8) - cam_x - CANVAS_BODY_X, (fr->y >> 8) - cam_y - CANVAS_BODY_Y);
  }
}

static void draw_hud(void) {
  int slots = 0;
  for (int i = 0; i < g_takes.slot_limit; i++) {
    if (g_takes.slots[i].active) slots |= 1 << i;
  }
  hud_draw(g_health, g_prog.max_health, g_prog.scrip, slots, g_takes.slot_limit, g_takes.recording,
           (frame_clock >> 4) & 1);
}

/* Music for the room Ruby is in. */
static void choose_music(void) {
  int region = pak_room(g_room_index)->region;
  current_region = region;
  int song = MUSIC_STAGE1;
  if (region == 0) song = MUSIC_BACKLOT;
  if (region == 2) song = MUSIC_STAGE2;
  if (g_room_index == ROOM_S1_CLOCK_TOP || g_room_index == ROOM_S2_ARENA) song = MUSIC_CHASE;
  if (song != current_song) {
    audio_music(song);
    current_song = song;
  }
}

/* Turn this tick's controller and game events into sound effects. */
static void play_sfx(void) {
  unsigned ev = g_hero.events;
  unsigned ge = g_events;
  if (ge & GE_DEATH) {
    audio_sfx(SFX_DEATH);
    return;
  }
  if (ev & (EV_JUMP | EV_WALLJUMP | EV_WIRE)) audio_sfx(SFX_JUMP);
  if (ev & EV_CLAP) audio_sfx(SFX_CLAP);
  if ((ev & EV_LAND) && !(ge & GE_ROOM)) audio_sfx(SFX_LAND);
  if (ge & GE_KILL) audio_sfx(SFX_KILL);
  else if (ge & GE_HIT) audio_sfx(SFX_HIT);
  if (ge & GE_COIN) audio_sfx(SFX_COIN);
  if (ge & GE_PICKUP) audio_sfx(SFX_PICKUP);
  if (ge & GE_HAZARD) audio_sfx(SFX_HAZARD);
  else if (ge & GE_HURT) audio_sfx(SFX_HURT);
  if (ge & GE_REC_START) audio_sfx(SFX_REC_START);
  if (ge & GE_REC_STOP) audio_sfx(SFX_REC_STOP);
  if (ge & GE_JUMPCUT) audio_sfx(SFX_JUMPCUT);
  if (ge & GE_SPRING) audio_sfx(SFX_SPRING);
  if (ge & GE_GATE) audio_sfx(SFX_GATE);
  if (ge & GE_SAVE) audio_sfx(SFX_SAVE);
  if (ge & GE_SLAM) audio_sfx(SFX_HAZARD);
  if (ge & GE_BOSS_DOWN) {
    audio_sfx(SFX_PICKUP);
    audio_music(MUSIC_STAGE1);
    current_song = MUSIC_STAGE1;
  }
}

/* Draw and show one frame of play. */
static void draw_frame(void) {
  render_frame(cam_x, cam_y);
  draw_entities();
  film_fx((int)frame_clock);
  draw_boss_bar();
  present();
  draw_sprites();
  draw_hud();
}

/* ---- title screen ---- */

static int title_screen(int have_save) {
  hud_hide();
  for (int s = 0; s < NUM_SPRITES; s++) sprite_hide(s);
  int choice = have_save ? 1 : 0;
  int ready = 0;
  while (1) {
    tl_copy_words((void*)g_pixel_fb, (void*)(g_pak + pak_header(PAKH_CARD_OFF)), (SCREEN_W * VIEW_H) >> 1);
    draw_text_centered(46, "SILVERLAKE PICTURES PRESENTS", FONT_SERIF, dim_ramp);
    draw_text_centered(66, "R E T A K E", FONT_TITLE, title_ramp);
    draw_text_centered(92, "a picture in eight reels", FONT_SERIF, dim_ramp);
    draw_text_centered(128, choice == 0 ? "> NEW PICTURE <" : "NEW PICTURE", FONT_SERIF,
                       choice == 0 ? title_ramp : dim_ramp);
    if (have_save) {
      draw_text_centered(144, choice == 1 ? "> CONTINUE <" : "CONTINUE", FONT_SERIF,
                         choice == 1 ? title_ramp : dim_ramp);
    }
    draw_text_centered(184, "A game by Claude Opus & GPT-6 Astra", FONT_SERIF, red_ramp);
    draw_text_centered(200, "Z jump   X clap   C record   V jump cut   E use", FONT_SERIF, dim_ramp);
    sprite_show(SLOT_HERO, SHEET_HERO, ((frame_clock >> 5) & 1) ? FR_IDLE1 : FR_IDLE0, 1, 64, 116);
    unsigned now = get_vga_frame_counter();
    while (get_vga_frame_counter() == now) {
    }
    frame_clock++;
    input_poll();
    int pressed = input_take_pressed();
    if (input_key_hit(KEY_ESC)) return -1;
    if ((frame_clock & 31) == 0) audio_heartbeat();
    if ((pressed & IN_UP) && choice != 0) {
      choice = 0;
      audio_sfx(SFX_SELECT);
    }
    if ((pressed & IN_DOWN) && have_save && choice != 1) {
      choice = 1;
      audio_sfx(SFX_SELECT);
    }
    if (frame_clock > 20) ready = 1;
    if (ready && (pressed & (IN_JUMP | IN_ATTACK | IN_INTERACT))) return choice;
  }
}

/* Debug launch: "retake room=N" starts a new game in room N (skipping the
 * title and intro) with every ability, for testing deep rooms. */
static int debug_room = -1;

static void parse_args(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    char* a = argv[i];
    if (a[0] == 'r' && a[1] == 'o' && a[2] == 'o' && a[3] == 'm' && a[4] == '=') {
      int n = 0;
      for (char* p = a + 5; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
      if (n < PAK_ROOM_COUNT) debug_room = n;
    }
  }
}

/* Play from the current game state until the player quits to the title. */
static void play(void) {
  update_camera(1);
  hud_bar_clear();
  choose_music();
  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int fps_frames = 0;
  while (1) {
    unsigned now = get_vga_frame_counter();
    while (now == last) now = get_vga_frame_counter();
    int ticks = (int)(now - last);
    if (ticks > 4) ticks = 4;
    last = now;
    input_poll();
    if (input_key_hit(KEY_ESC)) {
      if (pause_run()) return;
      last = get_vga_frame_counter();
      continue;
    }
    int held = input_held();
    int pressed = input_take_pressed();
    for (int t = 0; t < ticks; t++) {
      game_tick(held, pressed);
      pressed = 0;
      frame_clock++;
      play_sfx();
      if ((frame_clock & 31) == 0) audio_heartbeat();
      if (g_events & GE_ROOM) {
        update_camera(1);
        choose_music();
      }
      if (g_events & GE_SAVE) g_pending_script = SCRIPT_SAVED;
      if (g_pending_shop) {
        g_pending_shop = 0;
        shop_run();
        last = get_vga_frame_counter();
        break;
      }
      if (g_pending_script >= 0) {
        int script = g_pending_script;
        g_pending_script = -1;
        card_show(script);
        hud_bar_clear();
        last = get_vga_frame_counter();
        break;
      }
    }
    update_camera(0);
    draw_frame();
    fps_frames++;
    if (debug_room >= 0 && now - fps_mark >= 60) {
      hud_number(74, fps_frames * 60 / (int)(now - fps_mark), 3, 0);
      fps_frames = 0;
      fps_mark = now;
    }
  }
}

int main(int argc, char** argv) {
  parse_args(argc, argv);
  if (pak_open() != 0) return 1;
  audio_start();      /* fork the synthesizer before the big buffers exist */
  room_system_init();
  if (display_init() != 0) return 1;
  input_init();
  sprites_init();
  sheet_load(SHEET_HERO);
  sheet_load(SHEET_GHOST_A);
  sheet_load(SHEET_GHOST_B);

  while (1) {
    int choice = 2;
    if (debug_room < 0) {
      audio_music(MUSIC_TITLE);
      current_song = MUSIC_TITLE;
      int fd = open_existing("/retake/save.dat");
      int have_save = fd >= 0;
      if (fd >= 0) close(fd);
      choice = title_screen(have_save);
      if (choice < 0) break;
      audio_sfx(SFX_CLAP);
    }
    if (choice == 2) {
      game_new(debug_room);
      g_prog.abilities = AB_JUMPCUT | AB_WIRE | AB_GLIDE;
      g_hero.abilities = g_prog.abilities;
    } else if (choice == 1 && game_load() == 0) {
      /* continuing from the last mirror */
    } else {
      card_show(SCRIPT_INTRO);
      game_new(START_ROOM);
    }
    play();
    if (debug_room >= 0) break;
  }
  audio_stop();
  input_shutdown();
  display_shutdown();
  return 0;
}
