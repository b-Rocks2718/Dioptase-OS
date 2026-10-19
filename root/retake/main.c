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

/* Hero animation frames, in tools/ruby.py ORDER. */
#define FR_IDLE0 0
#define FR_IDLE1 1
#define FR_IDLE2 2
#define FR_IDLE3 3
#define FR_RUN0 4
#define FR_RUN1 5
#define FR_RUN2 6
#define FR_RUN3 7
#define FR_RUN4 8
#define FR_RUN5 9
#define FR_JUMP 10
#define FR_FALL 11
#define FR_FALL1 12
#define FR_LAND 13
#define FR_CLAP0 14
#define FR_CLAP1 15
#define FR_WALL 16
#define FR_HURT 17
#define FR_GLIDE 18

#define SLOT_HERO 0
#define SLOT_TAKE0 1

/* Where the 8x12 body's top-left sits on the 32x32 sprite canvas
 * (tools/ruby.py: CENTER_X 16, FEET_Y 30). */
#define CANVAS_BODY_X 12
#define CANVAS_BODY_Y 18

#define START_ROOM ROOM_BACKLOT_WEST

static int cam_x;
static int cam_y;
static unsigned frame_clock;
static int current_song = -1;
static int current_region = -1;

static short title_ramp[4] = {0, 0x345, 0xABC, 0xEFF};
static short dim_ramp[4] = {0, 0x123, 0x567, 0x89A};
static short red_ramp[4] = {0, 0x113, 0x238, 0x34F};

/* ---- game feel: dust, sparks, the landing squash, hit freeze, shake ----
 * Presentation only: none of this touches the rules in game.c, so routes,
 * replays and saves are unaffected. Puffs live in world space at 1/16 px. */
#define MAX_PUFFS 32
static void fill_rect(int x, int y, int w, int h, int color);
static void puff(int x_px, int y_px, int vx, int vy, int life, int color);
#define DUST 0x0CCD
#define SPARK 0x0AFF
#define GOLD 0x03CF

struct Puff {
  int x;
  int y;
  int vx;
  int vy;
  int life;      /* ticks left; 0 = free slot */
  int color;
};

static struct Puff puffs[32];
static int land_ticks;     /* ticks left to show the landing squash */
static int shake_ticks;    /* frames of screen shake left */
static int hitstop;        /* game ticks to hold still after a solid hit */
static int run_dust;       /* ticks since the last running dust puff */
static int pending_pressed; /* press edges not yet given to a game tick */
static int prev_kind[40];  /* each entity slot's kind last tick (0 = empty), to see deaths */
static int prev_cx[40];    /* and its center, where a death burst should appear */
static int prev_cy[40];
static int burst_dx[8] = {16, 11, 0, -11, -16, -11, 0, 11};
static int burst_dy[8] = {0, -11, -16, -11, 0, 11, 16, 11};

/* An enemy that vanished this tick bursts into dust and sparks where it
 * stood; a boss bursts bigger, in gold. */
static void death_bursts(int room_changed) {
  for (int i = 0; i < MAX_ENTS; i++) {
    struct Ent* e = &g_ents[i];
    int kind = (i < g_ent_count && e->alive) ? e->kind : 0;
    int was = prev_kind[i];
    if (!room_changed && was && kind != was) {
      if (was >= ENT_WALKER && was <= ENT_CHARGER) {
        for (int k = 0; k < 8; k++) puff(prev_cx[i], prev_cy[i], burst_dx[k], burst_dy[k], 14, DUST);
        for (int k = 0; k < 3; k++) puff(prev_cx[i], prev_cy[i], burst_dx[k * 3] >> 1, -12, 10, SPARK);
      } else if (was == ENT_BOSS) {
        for (int k = 0; k < 16; k++)
          puff(prev_cx[i], prev_cy[i], burst_dx[k & 7] * (1 + (k >> 3)), burst_dy[k & 7] * (1 + (k >> 3)), 20,
               (k & 1) ? GOLD : SPARK);
      }
    }
    prev_kind[i] = kind;
    if (kind) {
      prev_cx[i] = (e->x + (e->w >> 1)) >> 8;
      prev_cy[i] = (e->y + (e->h >> 1)) >> 8;
    }
  }
}

static void puff(int x_px, int y_px, int vx, int vy, int life, int color) {
  for (int i = 0; i < MAX_PUFFS; i++) {
    struct Puff* p = &puffs[i];
    if (p->life > 0) continue;
    p->x = x_px * 16;              /* may be negative near the left edge */
    p->y = y_px * 16;
    p->vx = vx;
    p->vy = vy;
    p->life = life;
    p->color = color;
    return;
  }
}

/* Turn this tick's events into dust, sparks, squash, freeze and shake, and
 * move the live puffs. Runs once after every game tick. */
static void feel_tick(void) {
  unsigned ev = g_hero.events;
  unsigned ge = g_events;
  int fx = (g_hero.x >> 8) + (PLAYER_W >> 1);
  int fy = (g_hero.y >> 8) + PLAYER_H;
  int face = g_hero.facing < 0 ? -1 : 1;
  if (ge & GE_ROOM) {
    for (int i = 0; i < MAX_PUFFS; i++) puffs[i].life = 0;
    land_ticks = 0;
  } else {
    if (ev & (EV_JUMP | EV_WIRE)) {
      puff(fx - 3, fy - 1, -7, -5, 14, DUST);
      puff(fx + 3, fy - 1, 7, -5, 14, DUST);
      puff(fx, fy - 1, 0, -2, 10, DUST);
    }
    if (ev & EV_WALLJUMP) {
      int wx = fx - face * 5;
      puff(wx, fy - 4, face * 6, -6, 14, DUST);
      puff(wx, fy - 9, face * 4, -3, 12, DUST);
    }
    if (ev & EV_LAND) {
      land_ticks = 5;
      for (int k = -2; k <= 2; k++) puff(fx + k * 2, fy - 1, k * 7, -3 - (k < 0 ? -k : k), 12, DUST);
    }
    if (g_hero.on_ground && (g_hero.vx > 384 || g_hero.vx < -384)) {
      if (++run_dust >= 9) {
        run_dust = 0;
        puff(fx - face * 3, fy - 1, -face * 5, -3, 10, DUST);
      }
    }
    if (ge & (GE_HIT | GE_KILL | GE_BOSS_HIT)) {
      hitstop = (ge & (GE_BOSS_HIT | GE_KILL)) ? 4 : 3;
      int hx = fx + face * 10;
      for (int k = 0; k < 6; k++) puff(hx, fy - 7, face * (6 + k * 3), -14 + k * 5, 10, SPARK);
    }
    if (ge & GE_COIN) {
      for (int k = 0; k < 4; k++) puff(fx, fy - 8, (k & 1 ? 1 : -1) * (4 + k * 3), -10 - k * 2, 14, GOLD);
    }
  }
  death_bursts((ge & GE_ROOM) != 0);
  if (ge & (GE_SLAM | GE_THUNDER)) shake_ticks = 10;
  if (ge & (GE_HURT | GE_HAZARD)) shake_ticks = 6;
  if (ge & GE_BOSS_DOWN) shake_ticks = 24;
  if (land_ticks > 0) land_ticks--;
  for (int i = 0; i < MAX_PUFFS; i++) {
    struct Puff* p = &puffs[i];
    if (p->life <= 0) continue;
    p->x += p->vx;
    p->y += p->vy;
    if (p->color == DUST) {
      p->vx -= p->vx >> 2;      /* dust drags to a stop and drifts up */
      if (p->vy > -3) p->vy--;
    } else {
      p->vy += 2;               /* sparks and coins fall */
    }
    p->life--;
  }
}

/* Puffs shrink from 2x2 to a single pixel as they fade. */
static void draw_puffs(void) {
  for (int i = 0; i < MAX_PUFFS; i++) {
    struct Puff* p = &puffs[i];
    if (p->life <= 0) continue;
    int size = p->life > 9 ? 3 : (p->life > 4 ? 2 : 1);
    fill_rect((p->x >> 4) - cam_x, (p->y >> 4) - cam_y, size, size, p->color);
  }
}

/* The six-frame run cycle over 64 px of travel (two strides): contact,
 * crouch and passing poses hold for 8-12 px each. */
static int run_seq[16] = {0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5};

/* Animation frame for a pose; running strides advance with distance. */
static int frame_for(int pose, int clapping_active, int x, int clock) {
  switch (pose) {
    case POSE_RUN: return FR_RUN0 + run_seq[(x >> 10) & 15];
    case POSE_RISE: return FR_JUMP;
    case POSE_FALL: return ((clock >> 3) & 1) ? FR_FALL1 : FR_FALL;
    case POSE_WALL: return FR_WALL;
    case POSE_GLIDE: return FR_GLIDE;
    case POSE_CLAP: return clapping_active ? FR_CLAP1 : FR_CLAP0;
    case POSE_HURT: return FR_HURT;
    default: return FR_IDLE0 + ((clock >> 4) & 3);   /* breathing, with the scarf stirring */
  }
}

/* Ease the camera toward Ruby (leading a little where she faces) and keep it
 * inside the room. The visible play area is pixel rows 8..235. */
#define SIDE_BAR 8   /* pixel-layer width of each black side bar (ui.c hud_hide) */

static void update_camera(int snap) {
  int hx = (g_hero.x >> 8) + (PLAYER_W >> 1);
  int hy = (g_hero.y >> 8) + (PLAYER_H >> 1);
  int tx = hx - (SCREEN_W >> 1) + g_hero.facing * 16;
  int ty = hy - 122;
  if (snap) {
    cam_x = tx;
    cam_y = ty;
  } else {
    cam_x += (tx - cam_x) >> 3;
    cam_y += (ty - cam_y) >> 3;
  }
  /* The black side bars cover 8 px at each screen edge, so the camera may
   * look 8 px past the room's sides: the room's edge then meets the bar
   * instead of hiding under it (and Ruby with it). */
  int max_x = g_room_pw - SCREEN_W + SIDE_BAR;
  int max_y = g_room_ph - VIEW_H;
  if (cam_x > max_x) cam_x = max_x;
  if (cam_y > max_y) cam_y = max_y;
  if (cam_x < -SIDE_BAR) cam_x = -SIDE_BAR;
  if (cam_y < -HUD_PIXEL_ROWS) cam_y = -HUD_PIXEL_ROWS;
  if (cam_y < 0) cam_y = 0;
}

/* Region 2 dresses the same behaviors as Western characters. */
static int region2_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER2_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER2_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER2_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER2_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER2_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return e->b ? IMG_COAL_0 : IMG_SHOT2_0;
    default: return -1;
  }
}

/* Region 3 dresses them as the pirate picture's cast. */
static int region3_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER3_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER3_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER3_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER3_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER3_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return IMG_SHOT3_0;
    case ENT_PLATFORM: return IMG_RAFT_0;
    default: return -1;
  }
}

/* Region 4: the jungle serial's menagerie. */
static int region4_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER4_0 + ((e->x >> 11) & 1);
    case ENT_HOPPER: return IMG_HOPPER4_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER4_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER4_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER4_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return IMG_SHOT4_0;
    default: return -1;
  }
}

/* Region 5: the haunted manor. */
static int region5_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER5_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER5_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER5_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER5_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER5_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return IMG_SHOT5_0;
    default: return -1;
  }
}

/* Region 6: the moon serial. */
static int region6_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER6_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER6_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER6_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER6_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER6_0 + ((frame_clock >> 2) & 1);
    case ENT_SHOT: return IMG_SHOT6_0;
    case ENT_PLATFORM: return IMG_PAD6_0;
    default: return -1;
  }
}


/* Fill a rectangle of the back buffer, clipped to the play area. */
static void fill_rect(int x, int y, int w, int h, int color) {
  int x1 = x + w;
  int y1 = y + h;
  if (x < 0) x = 0;
  if (y < HUD_PIXEL_ROWS) y = HUD_PIXEL_ROWS;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y1 > VIEW_H) y1 = VIEW_H;
  if (x >= x1 || y >= y1) return;
  view_touch(x, y, x1, y1);
  for (int yy = y; yy < y1; yy++) {
    unsigned short* row = g_back + g_row[yy];
    for (int xx = x; xx < x1; xx++) row[xx] = (unsigned short)color;
  }
}

/* A ray curtain: the emitter housing in the tile above its hitbox, then one
 * ray per tile of width. State 1 burns (bright, shimmering); state 2 is the
 * warning just before it lights (a faint flickering guide); state 0 is dark.
 * The hitbox is inset 2 px on each side of the curtain's tiles. */
static void draw_laser(struct Ent* e, int ex, int ey, int ew, int eh) {
  int x0 = ex - 2;
  int tiles = (ew + 4) >> 3;
  fill_rect(x0, ey - 7, tiles << 3, 6, 0x0444);
  fill_rect(x0, ey - 7, tiles << 3, 1, 0x0888);
  int lens = e->state == 1 ? 0x0C3E : (e->state == 2 && (frame_clock & 2) ? 0x0A2B : 0x0213);
  for (int k = 0; k < tiles; k++) {
    int rx = x0 + (k << 3) + 3;
    fill_rect(rx - 1, ey - 2, 4, 2, lens);
    if (e->state == 1) {
      fill_rect(rx - 1, ey, 1, eh, 0x0C3E);
      fill_rect(rx, ey, 1, eh, (frame_clock & 1) ? 0x0FFF : 0x0FDF);
      fill_rect(rx + 1, ey, 1, eh, 0x0FDF);
      fill_rect(rx + 2, ey, 1, eh, 0x0C3E);
    } else if (e->state == 2 && (frame_clock & 2)) {
      for (int y = ey + ((int)frame_clock & 3); y < ey + eh; y += 4) fill_rect(rx, y, 2, 1, 0x0A2B);
    }
  }
}

/* Brighten a back-buffer pixel toward a light's color (each channel halved,
 * then `tint` added; tint channels must stay at or below 8). */
static void light_px(int x, int y, int tint) {
  if (x < 0 || x >= SCREEN_W || y < HUD_PIXEL_ROWS || y >= VIEW_H) return;
  unsigned short* p = g_back + g_row[y] + x;
  *p = (unsigned short)(((*p >> 1) & 0x0777) + tint);
}

/* A searchlight: its lamp, then a dithered cone from the lamp to the spot
 * (e->vx), warmer while it is locked on someone (state 1) and dim while it
 * rests after a shot (state 2). */
static void draw_searchlight(struct Ent* e, int ex, int ey) {
  int lx = ex + 4;
  int ly = ey + 8;
  int sx = (e->vx >> 8) - cam_x;
  int rows = e->b << 3;
  int tint = e->state == 1 ? 0x0458 : (e->state == 2 ? 0x0333 : 0x0666);
  fill_rect(ex, ey + 1, 8, 6, 0x0333);
  fill_rect(ex + 1, ey + 6, 6, 2, e->state == 2 ? 0x0666 : 0x0EFF);
  if (rows <= 0) return;
  int left = (sx < lx ? sx : lx) - 14;
  int right = (sx > lx ? sx : lx) + 15;
  view_touch(left, ly, right, ly + rows + 2);
  int step_x = ((sx - lx) << 8) / rows;
  int half_q8 = 2 << 8;
  int step_half = (12 << 8) / rows;
  int cx_q8 = lx << 8;
  for (int r = 0; r < rows; r++) {
    int y = ly + r;
    if (y >= VIEW_H) break;
    int cx = cx_q8 >> 8;
    int half = half_q8 >> 8;
    if (y >= HUD_PIXEL_ROWS) {
      int x0 = cx - half + ((y + cx - half) & 1);
      int x1 = cx + half;
      if (x0 < 0) x0 = x0 & 1;               /* first on-screen pixel of the same dither parity */
      if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
      unsigned short* row = g_back + g_row[y];
      for (int x = x0; x <= x1; x += 2) row[x] = (unsigned short)(((row[x] >> 1) & 0x0777) + tint);
    }
    cx_q8 += step_x;
    half_q8 += step_half;
  }
  int fy = ly + rows;
  for (int x = sx - 13; x <= sx + 13; x++) {
    light_px(x, fy, tint);
    if (x > sx - 9 && x < sx + 9) light_px(x, fy + 1, tint);
  }
}

/* A slate: sticks open while it waits; shut, with a white bar draining
 * above it, while it holds its switch. The bar blinks for its last half
 * second so a player waiting at a gate knows to go. */
static void draw_slate(struct Ent* e, int ex, int ey, int ew, int eh) {
  int x = ex + ((ew - 16) >> 1);
  int y = ey + eh - 24;
  draw_image(IMG_SLATE_0 + (e->timer > 0 ? 1 : 0), x, y, 0);
  if (e->timer <= 0) return;
  int hold = e->b > 0 ? e->b : SLATE_HOLD;
  int fill = e->timer * 14 / hold;
  fill_rect(x + 1, y - 4, 14, 2, 0x0222);
  if (e->timer > 30 || (frame_clock & 4)) fill_rect(x + 1, y - 4, fill, 2, 0x0FFF);
}

/* A beat block: gold tiles with a star while solid, a dotted outline while
 * gone; either way it blinks through the last 18 ticks before it changes. */
static void draw_beat(struct Ent* e, int ex, int ey) {
  int tiles = e->a > 0 ? e->a : 1;
  int warn = e->timer <= 18 && (frame_clock & 4);
  for (int k = 0; k < tiles; k++) {
    int x = ex + (k << 3);
    if (e->state) {
      fill_rect(x, ey, 8, 8, warn ? 0x07EF : 0x03CF);
      fill_rect(x, ey + 7, 8, 1, 0x017A);
      fill_rect(x + 7, ey, 1, 8, 0x017A);
      fill_rect(x + 3, ey + 3, 2, 2, 0x0FFF);
    } else {
      int c = warn ? 0x03CF : 0x0158;
      for (int j = 0; j < 8; j += 2) {
        fill_rect(x + j, ey, 1, 1, c);
        fill_rect(x + j, ey + 7, 1, 1, c);
        fill_rect(x, ey + j, 1, 1, c);
        fill_rect(x + 7, ey + j, 1, 1, c);
      }
    }
  }
}

/* Night City's rain: short slanted streaks that fall a little each frame. */
static void draw_rain(void) {
  for (int k = 0; k < 44; k++) {
    int x = (k * 53 + (int)frame_clock * 2) & 511;
    int y = ((k * 97) + (int)frame_clock * 7) & 255;
    if (x >= SCREEN_W) continue;
    view_touch(x - 3, y, x + 1, y + 6);
    for (int j = 0; j < 6; j++) {
      int px = x - (j >> 1);
      int py = y + j;
      if (px >= 0 && py >= HUD_PIXEL_ROWS && py < VIEW_H) g_back[g_row[py] + px] = 0x0BA9;
    }
  }
}

/* While Rewind is ready, a flickering frame marks where it would put Ruby. */
static void draw_rewind_marker(void) {
  int rx;
  int ry;
  if (!(g_prog.abilities & AB_REWIND) || !game_rewind_target(&rx, &ry)) return;
  if (frame_clock & 4) return;
  int x = (rx >> 8) - cam_x;
  int y = (ry >> 8) - cam_y;
  fill_rect(x - 1, y - 1, 3, 1, 0x0FD8);
  fill_rect(x - 1, y - 1, 1, 3, 0x0FD8);
  fill_rect(x + PLAYER_W - 2, y - 1, 3, 1, 0x0FD8);
  fill_rect(x + PLAYER_W, y - 1, 1, 3, 0x0FD8);
  fill_rect(x - 1, y + PLAYER_H, 3, 1, 0x0FD8);
  fill_rect(x - 1, y + PLAYER_H - 2, 1, 3, 0x0FD8);
  fill_rect(x + PLAYER_W - 2, y + PLAYER_H, 3, 1, 0x0FD8);
  fill_rect(x + PLAYER_W, y + PLAYER_H - 2, 1, 3, 0x0FD8);
}
/* Region 7: the noir city. Straight-flying shots (b 2) are bullets. */
static int region7_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER7_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER7_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER7_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER7_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER7_0 + (e->state == 2 ? 1 : 0);
    case ENT_SHOT: return e->b == 2 ? IMG_BULLET7_0 : IMG_SHOT7_0;
    default: return -1;
  }
}

/* Region 8: the revue. */
static int region8_image(struct Ent* e) {
  switch (e->kind) {
    case ENT_WALKER: return IMG_WALKER8_0 + ((e->x >> 10) & 3);
    case ENT_HOPPER: return IMG_HOPPER8_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER8_0 + ((frame_clock >> 3) & 1);
    case ENT_THROWER: return IMG_THROWER8_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER8_0 + ((e->x >> 10) & 1);
    case ENT_SHOT: return IMG_SHOT8_0;
    case ENT_PLATFORM: return IMG_MOON8_0;
    default: return -1;
  }
}

/* The image for entity `e` this frame, or -1 to skip drawing. */
static int ent_image(struct Ent* e) {
  if (e->kind == ENT_SHOT && e->b == 2) return IMG_FILM_0;
  if (e->kind == ENT_SHOT && e->b == 3) return IMG_BOULDER_0;
  if (current_region == 4) {
    int v = region4_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 5) {
    int v = region5_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 6) {
    int v = region6_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 7) {
    int v = region7_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 8) {
    int v = region8_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 2) {
    int v = region2_image(e);
    if (v >= 0) return v;
  }
  if (current_region == 3) {
    int v = region3_image(e);
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
    case ENT_WALKER: return IMG_WALKER_0 + ((e->x >> 10) & 3);   /* a step every 4 px */
    case ENT_HOPPER: return IMG_HOPPER_0 + (e->vy != 0 ? 1 : 0);
    case ENT_FLYER: return IMG_FLYER_0 + ((frame_clock >> 2) & 3);  /* four wingbeat frames */
    case ENT_THROWER: return IMG_THROWER_0 + (e->state > 0 ? 1 : 0);
    case ENT_CHARGER: return IMG_CHARGER_0 + (e->state == 2 ? ((frame_clock >> 2) & 1) : 0);
    case ENT_SHOT: return IMG_SHOT_0;
    case ENT_SANDBAG: return IMG_SANDBAG_0;
    case ENT_SPRING: return IMG_SPRING_0 + (e->timer > 0 ? 1 : 0);
    case ENT_SHOP: return IMG_SHOP_0;
    case ENT_WAVE: return IMG_WAVE_0;
    case ENT_PLATFORM: return IMG_CART_0;
    case ENT_BOSS:
      if (e->a == 9) {
        if (e->state == 83) return IMG_BOSS9_3;
        if (e->state == 81 || e->state == 82) return IMG_BOSS9_2;
        if (e->state == 80) return (frame_clock & 8) ? IMG_BOSS9_2 : IMG_BOSS9_0;
        return IMG_BOSS9_0 + ((frame_clock >> 3) & 1);
      }
      if (e->a == 8) {
        if (e->state == 71 || e->state == 74) return IMG_BOSS8_1;
        if (e->state == 72) return (frame_clock & 4) ? IMG_BOSS8_2 : IMG_BOSS8_1;
        if (e->state == 73) return IMG_BOSS8_3;
        return IMG_BOSS8_0;
      }
      if (e->a == 7) {
        if (e->state == 61 || e->state == 62) return IMG_BOSS7_3;
        if (e->state == 63) return IMG_BOSS7_2;
        return IMG_BOSS7_0 + ((frame_clock >> 4) & 1);
      }
      if (e->a == 6) {
        if (e->state == 51 || e->state == 53) return IMG_BOSS6_2;
        if (e->state == 52) return IMG_BOSS6_3;
        return IMG_BOSS6_0 + ((frame_clock >> 4) & 1);
      }
      if (e->a == 5) {
        switch (e->state) {
          case 41: return IMG_BOSS5_2;
          case 42: return IMG_BOSS5_3;
          case 43: return IMG_BOSS5_4;
          default: return IMG_BOSS5_0 + ((frame_clock >> 4) & 1);
        }
      }
      if (e->a == 4) return (e->state == 31 ? IMG_BOSS4_2 : IMG_BOSS4_0) + ((frame_clock >> 3) & 1);
      if (e->a == 3) return -1;                  /* the Kraken is under the deck */
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
    if (e->kind == ENT_LASER) {
      draw_laser(e, ex, ey, ew, eh);
      continue;
    }
    if (e->kind == ENT_SEARCHLIGHT) {
      draw_searchlight(e, ex, ey);
      continue;
    }
    if (e->kind == ENT_BEAT) {
      draw_beat(e, ex, ey);
      continue;
    }
    if (e->kind == ENT_SLATE) {
      draw_slate(e, ex, ey, ew, eh);
      continue;
    }
    if (e->kind == ENT_FLYER && (current_region == 6 || current_region == 8)) {
      int wx = ex + (ew >> 1);                  /* the saucers fly on wires */
      if (wx >= 0 && wx < SCREEN_W) {
        view_touch(wx, HUD_PIXEL_ROWS, wx + 1, ey);
        for (int y = HUD_PIXEL_ROWS; y < ey && y < VIEW_H; y++) g_back[g_row[y] + wx] = 0x0999;
      }
    }
    if (e->kind == ENT_PENDULUM) {
      int px = (e->tx << 3) + 4 - cam_x;
      int py = (e->ty << 3) + 4 - cam_y;
      int bx = ex + 6;
      int by = ey + 4;
      int n = (e->a > 0 ? e->a : 6) << 1;     /* a chain link every 4 px */
      int sx = (bx - px) * 256 / n;
      int sy = (by - py) * 256 / n;
      int rx = px * 256;
      int ry = py * 256;
      view_touch(px < bx ? px : bx, py < by ? py : by, (px > bx ? px : bx) + 1, (py > by ? py : by) + 1);
      for (int k = 0; k < n; k++) {
        int x = rx >> 8;
        int y = ry >> 8;
        if (x >= 0 && x < SCREEN_W && y >= HUD_PIXEL_ROWS && y < VIEW_H) g_back[g_row[y] + x] = 0x0CBC;
        rx += sx;
        ry += sy;
      }
      draw_image(IMG_BLADE_0, bx - 7, by - 3, 0);
      continue;
    }
    if (e->kind == ENT_BOLT) {
      int bx = ex + 5;
      int floor = e->c - cam_y;
      if (e->state == 0 && current_region == 7) {
        int cy = floor - 7;
        for (int k = -5; k <= 5; k++) {
          if (k > -2 && k < 2) continue;
          fill_rect(bx + k, cy, 1, 1, 0x022F);
          fill_rect(bx, cy + k, 1, 1, 0x022F);
        }
      } else if (e->state != 0 && current_region == 7) {
        fill_rect(bx, HUD_PIXEL_ROWS, 1, floor - HUD_PIXEL_ROWS, 0x0FFF);
        fill_rect(bx - 2, floor - 3, 5, 3, 0x0AEF);
      } else if (e->state == 0) {
        if (frame_clock & 2) {
          view_touch(bx - 3, floor - 4, bx + 4, floor);
          for (int k = -3; k <= 3; k++) {
            int x = bx + k;
            int y = floor - 1 - (k < 0 ? -k : k);
            if (x >= 0 && x < SCREEN_W && y >= HUD_PIXEL_ROWS && y < VIEW_H) g_back[g_row[y] + x] = 0x0FFF;
          }
        }
      } else {
        int jag = 0;
        int edge = current_region == 6 ? 0x0C3E : 0x0EDE;   /* the Emperor's rays are magenta */
        view_touch(bx - 3, HUD_PIXEL_ROWS, bx + 4, floor);
        for (int y = HUD_PIXEL_ROWS; y < floor && y < VIEW_H; y++) {
          if ((y & 7) == 0) jag = ((y * 7 + (int)frame_clock) & 3) - 1;
          for (int w = -1; w <= 1; w++) {
            int x = bx + jag + w;
            if (x >= 0 && x < SCREEN_W) g_back[g_row[y] + x] = w ? edge : 0x0FFF;
          }
        }
      }
      continue;
    }
    if (e->kind == ENT_CRUMBLE) {
      if (e->state == 0) draw_image(IMG_CRUMBLE_0, ex, ey, 0);
      else if (e->state == 1) draw_image(IMG_CRUMBLE_1, ex + ((frame_clock & 2) ? 1 : -1), ey, 0);
      else if (e->anim < 24) draw_image(IMG_CRUMBLE_1, ex, ey + ((e->anim * e->anim) >> 3), 0);
      continue;
    }
    if (e->kind == ENT_TENTACLE) {
      int cx = e->b - cam_x;
      int deck = (e->c >> 8) - cam_y;
      if (e->state == 0) {
        if (frame_clock & 4) draw_image(IMG_BUBBLES_0, cx - 8, deck - 8, 0);
      } else if (e->state == 1 || e->state == 2) {
        int rise = e->state == 1 ? e->timer * 4 : 0;   /* emerging from the deck */
        draw_image_clipped(IMG_TENTACLE_UP_0, cx - 8, deck - 64 + rise, 0, deck);
      } else if (e->state == 5) {
        draw_image_clipped(IMG_TENTACLE_UP_0, cx - 8, deck - 64 + (12 - e->timer) * 5, 0, deck);
      } else {
        draw_image(IMG_TENTACLE_DOWN_0, e->dir < 0 ? cx - 62 : cx - 2, deck - 14, e->dir > 0);
      }
      continue;
    }
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
                              e->kind == ENT_CHARGER || e->kind == ENT_HOPPER ||
                              (e->kind == ENT_BOSS && e->a != 4));
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
  int total = g_boss->a == 9 ? 26 : g_boss->a == 8 ? 24 : g_boss->a == 7 ? 22 : g_boss->a == 6 ? 20 : g_boss->a == 5 ? 20 : g_boss->a == 4 ? 18 : (g_boss->a == 3 ? 12 : (g_boss->a == 2 ? 10 : 16));
  int hp = g_boss->hp < 0 ? 0 : g_boss->hp;
  int x0 = 80;
  int w = 160;
  int filled = (w * hp) / total;
  view_touch(x0 - 1, 226, x0 + w + 1, 232);
  for (int y = 226; y < 232; y++) {
    unsigned short* row = g_back + g_row[y];
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
  if (land_ticks > 0 && (g_hero.pose == POSE_IDLE || g_hero.pose == POSE_RUN)) f = FR_LAND;
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
  hud_text(64, g_overcrank > 0 ? "OVERCRANK" : "         ", 0);
}

/* Music for the room Ruby is in. */
static void choose_music(void) {
  int region = pak_room(g_room_index)->region;
  current_region = region;
  int song = MUSIC_STAGE1;
  if (region == 0) song = MUSIC_BACKLOT;
  if (region == 2) song = MUSIC_STAGE2;
  if (region == 3) song = MUSIC_STAGE3;
  if (region == 4) song = MUSIC_STAGE4;
  if (region == 5) song = MUSIC_STAGE5;
  if (region == 6) song = MUSIC_STAGE6;
  if (region == 7) song = MUSIC_STAGE7;
  if (region == 8) song = MUSIC_STAGE8;   /* the show goes on, even through the Maestro's fight */
  if (g_room_index == ROOM_S1_CLOCK_TOP || g_room_index == ROOM_S2_ARENA || g_room_index == ROOM_S3_ARENA ||
      g_room_index == ROOM_TOWER_BOOTH || g_room_index == ROOM_S4_ARENA || g_room_index == ROOM_S5_ARENA ||
      g_room_index == ROOM_S6_THRONE || g_room_index == ROOM_S7_PENTHOUSE) {
    song = MUSIC_CHASE;
  }
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
  if (ge & GE_THUNDER) audio_sfx(SFX_HAZARD);
  if (ge & GE_OVERCRANK) audio_sfx(SFX_REC_START);
  if (ge & GE_REWIND) audio_sfx(SFX_JUMPCUT);
  if (ge & GE_BOSS_DOWN) {
    audio_sfx(SFX_PICKUP);
    audio_music(MUSIC_STAGE1);
    current_song = MUSIC_STAGE1;
  }
}

/* Draw and show one frame of play, shaken if something heavy just landed. */
static void draw_frame(void) {
  int keep_x = cam_x;
  int keep_y = cam_y;
  if (shake_ticks > 0) {
    shake_ticks--;
    cam_y += (shake_ticks & 2) ? 2 : -2;
    cam_x += (shake_ticks & 4) ? 1 : -1;
    int max_x = g_room_pw - SCREEN_W + SIDE_BAR;
    int max_y = g_room_ph > VIEW_H ? g_room_ph - VIEW_H : 0;
    if (cam_x > max_x) cam_x = max_x;
    if (cam_y > max_y) cam_y = max_y;
    if (cam_x < -SIDE_BAR) cam_x = -SIDE_BAR;
    if (cam_y < 0) cam_y = 0;
  }
  view_begin(cam_x, cam_y);
  draw_entities();
  draw_puffs();
  if (current_region == 7) draw_rain();
  draw_rewind_marker();
  film_fx((int)frame_clock);
  draw_boss_bar();
  view_end();
  draw_sprites();
  draw_hud();
  cam_x = keep_x;
  cam_y = keep_y;
}

/* ---- title screen ---- */

static int title_screen(int have_save) {
  hud_hide();
  for (int s = 0; s < NUM_SPRITES; s++) sprite_hide(s);
  int choice = have_save ? 1 : 0;
  int ready = 0;
  int drawn = -1;
  while (1) {
    /* Composed off screen and redrawn only when the selection changes;
     * redrawing it on the display every frame made it flicker. */
    if (drawn != choice) {
      drawn = choice;
      canvas_card();
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
      canvas_show();
    }
    sprite_show(SLOT_HERO, SHEET_HERO, FR_IDLE0 + ((frame_clock >> 4) & 3), 1, 64, 116);
    unsigned now = get_vga_frame_counter();
    while (get_vga_frame_counter() == now) {
      sleep(1);        /* idle the core until the next frame instead of spinning */
    }
    frame_clock++;
    input_poll();
    int pressed = input_take_pressed();
    if (input_key_hit('q')) return -1;
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

/* The Camera is broken: the ending, the credits, then back to the title.
 * Progress is saved first so the finished picture shows on Continue. */
static void finale(void) {
  if (game_save() != 0)
    card_message("RETAKE - SAVE FAILED\nThe finished picture could not be saved.\nAny earlier checkpoint is preserved.\nCheck disk space before trying again.");
  audio_music(MUSIC_TITLE);
  current_song = MUSIC_TITLE;
  card_show(SCRIPT_ENDING);
  card_show(SCRIPT_CREDITS);
}

/* Play from the current game state until the player quits to the title. */
static void play(void) {
  pending_pressed = 0;
  hitstop = 0;
  update_camera(1);
  hud_bar_clear();
  choose_music();
  unsigned last = get_vga_frame_counter();
  unsigned fps_mark = last;
  int fps_frames = 0;
  while (1) {
    unsigned now = get_vga_frame_counter();
    /* Sleep a jiffy (about 1 ms) between looks at the frame counter rather
     * than spinning on it: the idle core is then free for the synthesizer
     * and the terminal, and the emulator can park it. */
    while (now == last) {
      sleep(1);
      now = get_vga_frame_counter();
    }
    int ticks = (int)(now - last);
    if (ticks > 4) ticks = 4;
    last = now;
    input_poll();
    if (input_key_hit('p')) {           /* not Esc: the emulator window closes on Esc */
      if (pause_run()) return;
      pending_pressed = 0;
      last = get_vga_frame_counter();
      continue;
    }
    int held = input_held();
    /* Press edges wait here until a game tick consumes them, so a tap made
     * while a hit freeze skips every tick of a frame is not lost. */
    pending_pressed |= input_take_pressed();
    for (int t = 0; t < ticks; t++) {
      if (hitstop > 0) {            /* a solid hit holds the picture for a beat */
        hitstop--;
        continue;
      }
      game_tick(held, pending_pressed);
      pending_pressed = 0;
      frame_clock++;
      play_sfx();
      feel_tick();
      if (g_events & GE_FINALE) {
        finale();
        return;
      }
      if ((frame_clock & 31) == 0) audio_heartbeat();
      if (g_events & GE_ROOM) {
        update_camera(1);
        choose_music();
      }
      if (g_events & GE_SAVE) g_pending_script = SCRIPT_SAVED;
      if (g_events & GE_SAVE_FAILED) {
        card_message("RETAKE - SAVE FAILED\nThe mirror could not store your picture.\nAny earlier checkpoint is preserved.\nTry again before leaving the picture.\nIf it still fails, check disk space.");
        hud_bar_clear();
        last = get_vga_frame_counter();
        break;
      }
      if (g_pending_shop) {
        g_pending_shop = 0;
        shop_run();
        pending_pressed = 0;
        last = get_vga_frame_counter();
        break;
      }
      if (g_pending_script >= 0) {
        int script = g_pending_script;
        g_pending_script = -1;
        card_show(script);
        hud_bar_clear();
        pending_pressed = 0;
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
      choice = title_screen(game_has_save());
      if (choice < 0) break;
      audio_sfx(SFX_CLAP);
    }
    if (choice == 2) {
      game_new(debug_room);
      g_prog.abilities = AB_JUMPCUT | AB_WIRE | AB_GLIDE;
      g_hero.abilities = g_prog.abilities;
    } else if (choice == 1) {
      if (game_load() != 0) {
        card_message("RETAKE - COULD NOT CONTINUE\nNeither checkpoint could be loaded.\nYour save files are unchanged.\nReturning to the title.");
        continue;
      }
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
