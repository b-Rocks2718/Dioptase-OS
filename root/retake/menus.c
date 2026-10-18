#include "../crt/sys.h"
#include "../crt/vga.h"
#include "retake.h"
#include "game.h"
#include "pak.h"

/*
 * Full-screen menus drawn on the intertitle card: the Prop Department shop
 * and the Intermission (pause) screen. Both run their own small loops that
 * poll input, keep the synthesizer's heartbeat going, and return to the game.
 */

static short title_ramp[4] = {0, 0x345, 0xABC, 0xEFF};
static short text_ramp[4] = {0, 0x234, 0x9AB, 0xDEF};
static short dim_ramp[4] = {0, 0x123, 0x456, 0x789};
static short gold_ramp[4] = {0, 0x123, 0x38B, 0x6CF};

static void card_background(void) {
  tl_copy_words((void*)g_pixel_fb, (void*)(g_pak + pak_header(PAKH_CARD_OFF)), (SCREEN_W * VIEW_H) >> 1);
}

/* Wait one display frame; keep the synthesizer alive. */
static void menu_frame(unsigned* beat) {
  unsigned now = get_vga_frame_counter();
  while (get_vga_frame_counter() == now) {
  }
  if (now - *beat >= 30) {
    audio_heartbeat();
    *beat = now;
  }
  input_poll();
}

static void number_text(char* buf, int value) {
  char tmp[12];
  int n = 0;
  if (value == 0) tmp[n++] = '0';
  while (value > 0 && n < 11) {
    tmp[n++] = (char)('0' + value % 10);
    value = value / 10;
  }
  int i = 0;
  while (n > 0) buf[i++] = tmp[--n];
  buf[i] = 0;
}

/* Copy src after dst's NUL; returns dst. */
static char* append(char* dst, char* src) {
  char* d = dst;
  while (*d) d++;
  while (*src) *d++ = *src++;
  *d = 0;
  return dst;
}

/* ---- the Prop Department ---- */

#define SHOP_ITEMS 5

static char* shop_names[5] = {
  "A second take reel",
  "A third take reel",
  "Longer film stock",
  "Even longer film stock",
  "A spare film can (+1 frame)",
};
static int shop_prices[5] = {25, 60, 40, 70, 50};

/* Whether item i can be bought now (0), is sold out (1), or needs an
 * earlier item first (2). */
static int shop_state(int i) {
  switch (i) {
    case 0: return g_prog.take_slots >= 2 ? 1 : 0;
    case 1: return g_prog.take_slots >= 3 ? 1 : (g_prog.take_slots < 2 ? 2 : 0);
    case 2: return g_prog.take_frames >= 360 ? 1 : 0;
    case 3: return g_prog.take_frames >= 480 ? 1 : (g_prog.take_frames < 360 ? 2 : 0);
    default: return g_prog.max_health >= 8 ? 1 : 0;
  }
}

static void shop_buy(int i) {
  switch (i) {
    case 0: g_prog.take_slots = 2; break;
    case 1: g_prog.take_slots = 3; break;
    case 2: g_prog.take_frames = 360; break;
    case 3: g_prog.take_frames = 480; break;
    default:
      g_prog.max_health++;
      g_health = g_prog.max_health;
      break;
  }
  takes_init(&g_takes, g_prog.take_slots, g_prog.take_frames);
}

void shop_run(void) {
  for (int s = 0; s < NUM_SPRITES; s++) sprite_hide(s);
  hud_hide();
  int sel = 0;
  int message = 0;   /* 0 greeting, 1 bought, 2 cannot afford, 3 sold out, 4 locked */
  int dirty = 1;     /* redraw only on change: drawing straight to the display flickers */
  unsigned beat = get_vga_frame_counter();
  input_take_pressed();
  while (1) {
    if (dirty) {
    dirty = 0;
    card_background();
    draw_text_centered(24, "THE PROP DEPARTMENT", FONT_TITLE, title_ramp);
    char* line = "\"Otto, props. Everything here survived the camera. Mostly.\"";
    if (message == 1) line = "\"Pleasure doing business. Mind the edges, they're sharp.\"";
    if (message == 2) line = "\"Short on scrip, kid. The lot's full of it, if you look.\"";
    if (message == 3) line = "\"That's all I've got of those. Come back later.\"";
    if (message == 4) line = "\"One thing at a time, kid. Buy the one above it first.\"";
    draw_text_centered(52, line, FONT_SERIF, dim_ramp);
    for (int i = 0; i < SHOP_ITEMS; i++) {
      int y = 80 + i * 20;
      int st = shop_state(i);
      short* ramp = st == 0 ? text_ramp : dim_ramp;
      if (i == sel) draw_text(40, y, ">", FONT_SERIF, gold_ramp);
      draw_text(54, y, shop_names[i], FONT_SERIF, ramp);
      char price[16];
      price[0] = 0;
      if (st == 1) append(price, "SOLD");
      else if (st == 2) append(price, "---");
      else number_text(price, shop_prices[i]);
      draw_text(256, y, price, FONT_SERIF, st == 0 ? gold_ramp : dim_ramp);
    }
    char purse[32];
    purse[0] = 0;
    append(purse, "Your scrip: ");
    char num[12];
    number_text(num, g_prog.scrip);
    append(purse, num);
    draw_text_centered(190, purse, FONT_SERIF, gold_ramp);
    draw_text_centered(206, "up/down choose   Z buy   X leave", FONT_SERIF, dim_ramp);
    }
    menu_frame(&beat);
    int p = input_take_pressed();
    if (input_key_hit(KEY_ESC) || (p & IN_ATTACK)) break;
    if (p) dirty = 1;
    if ((p & IN_UP) && sel > 0) {
      sel--;
      audio_sfx(SFX_SELECT);
    }
    if ((p & IN_DOWN) && sel < SHOP_ITEMS - 1) {
      sel++;
      audio_sfx(SFX_SELECT);
    }
    if (p & (IN_JUMP | IN_INTERACT)) {
      int st = shop_state(sel);
      if (st != 0) {
        message = st == 2 ? 4 : 3;
      } else if (g_prog.scrip < shop_prices[sel]) {
        message = 2;
      } else {
        g_prog.scrip -= shop_prices[sel];
        shop_buy(sel);
        message = 1;
        audio_sfx(SFX_PICKUP);
      }
    }
  }
  input_take_pressed();
  hud_bar_clear();
}

/* ---- Intermission (pause) ---- */

/* Reels collected and total, counted from the pak's room entities. */
static void reel_counts(int* found, int* total) {
  *found = 0;
  *total = 0;
  int n = pak_room_count();
  for (int i = 0; i < n; i++) {
    struct PakRoom* r = pak_room(i);
    short* raw = (short*)(g_pak + r->entities_off);
    for (int k = 0; k < r->entity_count; k++) {
      short* e = raw + (k << 3);
      if (e[0] != ENT_REEL) continue;
      (*total)++;
      if (flag_get(e[7])) (*found)++;
    }
  }
}

/* Returns 1 if the player chose to quit to the title. */
int pause_run(void) {
  for (int s = 0; s < NUM_SPRITES; s++) sprite_hide(s);
  hud_hide();
  int found;
  int total;
  reel_counts(&found, &total);
  unsigned beat = get_vga_frame_counter();
  input_take_pressed();
  int quit = 0;
  card_background();
  {
    draw_text_centered(30, "INTERMISSION", FONT_TITLE, title_ramp);
    char line[64];
    char num[12];
    line[0] = 0;
    append(line, "Lost reels found: ");
    number_text(num, found);
    append(line, num);
    append(line, " of ");
    number_text(num, total);
    append(line, num);
    draw_text_centered(70, line, FONT_SERIF, text_ramp);
    line[0] = 0;
    append(line, "Scrip: ");
    number_text(num, g_prog.scrip);
    append(line, num);
    append(line, "     Frames: ");
    number_text(num, g_prog.max_health);
    append(line, num);
    draw_text_centered(88, line, FONT_SERIF, text_ramp);
    line[0] = 0;
    append(line, "Time on set: ");
    unsigned minutes = g_prog.play_ticks / 3600;
    number_text(num, (int)minutes);
    append(line, num);
    append(line, " min");
    draw_text_centered(106, line, FONT_SERIF, text_ramp);
    line[0] = 0;
    append(line, "Abilities: clap, record");
    if (g_prog.abilities & AB_WIRE) append(line, ", stunt wire");
    if (g_prog.abilities & AB_JUMPCUT) append(line, ", jump cut");
    if (g_prog.abilities & AB_GLIDE) append(line, ", umbrella");
    draw_text_centered(124, line, FONT_SERIF, dim_ramp);
    draw_text_centered(160, "Esc or Z: back to the picture", FONT_SERIF, gold_ramp);
    draw_text_centered(176, "Q: quit to the title", FONT_SERIF, dim_ramp);
    draw_text_centered(190, "(anything since the last mirror is lost)", FONT_SERIF, dim_ramp);
  }
  while (1) {
    menu_frame(&beat);
    int p = input_take_pressed();
    if (input_key_hit('q')) {
      quit = 1;
      break;
    }
    if (input_key_hit(KEY_ESC) || (p & (IN_JUMP | IN_INTERACT))) break;
  }
  input_take_pressed();
  hud_bar_clear();
  return quit;
}
