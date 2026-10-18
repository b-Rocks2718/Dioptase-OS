#ifndef RETAKE_H
#define RETAKE_H

#include "player.h"

/*
 * RETAKE shared declarations. A user-mode program on Dioptase-OS.
 *
 * Display plan (docs/mem_map.md for the formats; COLLAB.md for the
 * emulator-specific behavior relied on):
 *   pixel layer  - composited each frame: parallax backdrop + room image +
 *                  software-drawn entities and particles
 *   tile layer   - tile scale 0 (8x8 tiles at 640x480): HUD, letterbox bars
 *                  and text overlays drawn above the game
 *   sprites      - the stunt double and her takes (scale 1, 320-space)
 */

#define SCREEN_W 320
#define SCREEN_H 240
#define VIEW_H 236             /* pixel rows user mode can map (PIXEL_FB_USER_PIXELS) */
#define ROOM_MAX_W 80
#define ROOM_MAX_H 60
#define ROOM_MAX_PW 640        /* ROOM_MAX_W * TILE */
#define ROOM_MAX_PH 480
#define ROOM_MAX_TILES 4800     /* ROOM_MAX_W * ROOM_MAX_H; bcc array sizes must be literals */
#define TRANSPARENT_PX 0xF000

/* Key codes (docs/mem_map.md, PS/2 keyboard input stream). */
#define KEY_ESC 0x1B
#define KEY_ENTER 0x0D
#define KEY_SPACE 0x20
#define KEY_RIGHT 0x85
#define KEY_LEFT 0x86
#define KEY_DOWN 0x87
#define KEY_UP 0x88

/* ---- pak.c: the read-only asset pack (layout in tools/build.py) ---- */

struct PakRegion {
  unsigned block_count;
  unsigned blocks_off;
  unsigned backdrop_w;
  unsigned backdrop_h;
  unsigned backdrop_off;
};

struct PakRoom {
  int region;
  int wx;
  int wy;
  int w;
  int h;
  int blocks_off;
  int col_off;
  int entity_count;
  int entities_off;
  int spawn_x;
  int spawn_y;
  int name_off;
};

struct PakSprite {
  unsigned frame_count;
  unsigned frames_off;
};

extern unsigned char* g_pak;

int pak_open(void);
int pak_room_count(void);
struct PakRoom* pak_room(int index);
struct PakRegion* pak_region(int index);
struct PakSprite* pak_sprite(int index);

/* ---- input.c ---- */

void input_init(void);
void input_shutdown(void);
void input_poll(void);
int input_held(void);
int input_take_pressed(void);
int input_key_down(int code);
int input_key_hit(int code);

/* ---- room.c: the current room ---- */

extern struct Room g_room;
extern int g_room_index;
extern int g_room_pw;          /* room size in pixels */
extern int g_room_ph;
extern unsigned short* g_room_img;      /* composed room image, row stride g_room_pw */
extern unsigned short* g_room_img_odd;  /* the same shifted left by one pixel */

void room_system_init(void);
void room_load(int index);

/* ---- render.c ---- */

extern volatile short* g_pixel_fb;
extern volatile short* g_tile_fb;
extern volatile short* g_tilemap;
extern volatile short* g_spritemap;

int display_init(void);
void display_shutdown(void);
void render_region(int region_index);
void render_frame(int cam_x, int cam_y);
void hero_sprite_init(void);
void hero_sprite_place(int frame, int facing, int x, int y);

void tl_copy_words(void* dst, void* src, int words);
void rt_composite(void* dst, void* room_row, void* bg_row, int words);

#endif
