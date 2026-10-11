#ifndef DELVE_H
#define DELVE_H

/*
 * DELVE -- a real-time first-person dungeon crawler for Dioptase.
 *
 * Shared declarations for the game modules:
 *   main.c   display ownership, main loop, input, game states
 *   render.c raycaster, billboard sprites, view effects
 *   world.c  level generation, map queries, monsters, items, projectiles
 *   hud.c    status bar, text, minimap, title/overlay screens
 *   audio.c  synthesizer process (music and sound effects)
 *   gfx.s    hand-written per-pixel loops and fast arithmetic helpers
 *   assets.s generated palette, shade tables, textures, sprites, fonts
 *
 * Numbers: world positions are 16.16 fixed point with one map cell = 1.0.
 * Angles are 12-bit (4096 per turn): 0 faces +x (east), 1024 faces +y
 * (south, because map rows grow downward), 2048 west, 3072 north.
 */

#include "../crt/stdbool.h"
#include "../crt/stddef.h"

/* ---- Fixed point ---- */
#define FIX_ONE 65536
#define FIX_HALF 32768
#define ANGLE_COUNT 4096
#define ANGLE_MASK 4095
#define ANGLE_QUARTER 1024

/* ---- View layout ----
 * The 3D view is 160x96 logical pixels shown at 4x (640x384 screen pixels)
 * by tiles 0..239 of the tile layer at tile scale 2 (docs/mem_map.md). Tiles
 * are assigned column-major (tile index = tx * 12 + ty), so a pixel's byte
 * offset in both the back buffer and the tilemap is
 *   (x >> 3) * 1536 + (x & 7) * 2 + y * 16.
 * The 160x24 status bar below it is five 32x32 hardware sprites at scale 2.
 */
#define VIEW_W 160
#define VIEW_H 96
#define VIEW_HALF_H 48
#define VIEW_TILE_COLS 20
#define VIEW_TILE_ROWS 12
#define VIEW_TILES 240
#define VIEW_SHORTS 15360
#define VIEW_COL_GROUP_SHORTS 768   /* shorts per 8-pixel-wide tile column */
#define VIEW_ROW_SHORTS 8           /* shorts between vertically adjacent pixels */

/* Projection: wall height in pixels at perpendicular distance d is
 * PROJ_K / d. PROJ_K = (VIEW_W / 2) / PLANE_LEN with PLANE_LEN = 0.7, a
 * horizontal field of view of about 70 degrees. */
#define PLANE_LEN 45875             /* 0.7 in 16.16 */
#define PROJ_K 114

/* ---- Tile layer resources ---- */
#define TILE_SCALE_4X 2
#define BLANK_TILE 255
#define TILE_FB_COLS 80
#define TILE_FB_ROWS 60
#define TILEMAP_SHORTS_PER_TILE 64

/* ---- Hardware sprites ----
 * Sprite coordinates are multiplied by the sprite's scale on screen, so a
 * scale-2 sprite at (x, y) appears at screen (4x, 4y). */
#define SPRITE_SHORTS 1024
#define HUD_SPRITE_FIRST 11         /* sprites 11..15: status bar strip */
#define HUD_SPRITE_COUNT 5
#define HUD_Y 96                    /* logical row of the status bar */
#define HUD_H 24
#define HUD_W 160
#define HUD_PIXELS 3840          /* HUD_W * HUD_H (array bounds must be literals) */

/* Drawing surfaces for the 2D helpers in hud.c. */
#define SURF_VIEW 0
#define SURF_HUD 1
#define SPRITE_HIDDEN 1000          /* off-screen coordinate (vga.h) */
#define TRANSPARENT_PIXEL 0xF000

/* ---- Lighting ---- */
#define LIGHT_LEVELS 16
#define LIGHT_FULL 11
#define SHADE_TABLE_SHORTS 256

/* ---- Map ---- */
#define MAP_W 32
#define MAP_H 32
#define MAP_CELLS 1024

/* Cell contents: 0 is open floor. Values 1..15 are solid walls using texture
 * (value - 1). */
#define CELL_EMPTY 0

/* ---- Textures (assets.s order) ---- */
#define TEX_SIZE 64
#define TEX_BYTES 4096
#define TEX_BRICK 0
#define TEX_BRICK_MOSS 1
#define TEX_ROCK 2
#define TEX_ROCK_GEM 3
#define TEX_DOOR 4
#define TEX_DOOR_LOCKED 5
#define TEX_RUNES 6
#define TEX_BRICK_DARK 7

/* ---- Camera ---- */
struct Camera {
  int x;
  int y;
  int angle;
  int bob;       /* vertical bob in view pixels (head motion while walking) */
};

/* Per-row floor casting state for draw_flats_asm() in gfx.s (field order is
 * ABI: 24 bytes per record). */
struct FlatRow {
  int u;
  int v;
  int du;
  int dv;
  unsigned short* floor_shade;
  unsigned short* ceil_shade;
};

/* ---- gfx.s ---- */
void blit_view(volatile short* dst, unsigned short* src);
void copy_words(void* dst, void* src, unsigned words);
void draw_wall(unsigned short* dst, int count, unsigned char* texcol,
               int texpos, int texstep, unsigned short* shade);
void fill_rows(unsigned short* dst, int count, unsigned short* colors, int advance);
unsigned umul16(unsigned a, unsigned b);
unsigned udiv32(unsigned n, unsigned d);
void draw_flats_asm(struct FlatRow* rows, unsigned short* view,
                    unsigned char* floor_tex, unsigned char* ceil_tex);

/* ---- assets.s ---- */
extern unsigned short shade_tables[4096];
extern unsigned char wall_textures[32768];
extern unsigned char flat_textures[16384];
#define FLAT_FLOOR_STONE 0
#define FLAT_CEILING_BEAMS 1
#define FLAT_FLOOR_MOSS 2
#define FLAT_CEILING_ROCK 3
extern int sin_table[4096];
extern unsigned char font_small[320];
extern unsigned char font_big[768];
extern unsigned char font_big_spans[192];

/* ---- main.c ---- */
extern volatile short* g_tilemap;
extern volatile short* g_tile_fb;
extern volatile short* g_spritemap;
/* The view and status-bar buffers are copied with word loads/stores, so they
 * are declared as word arrays (bcc only 2-aligns short arrays). */
extern unsigned g_view_words[7680];
#define g_view ((unsigned short*)g_view_words)
extern unsigned g_frame;            /* VGA frames since start (60 Hz) */

/* ---- render.c ---- */
int isin(int angle);
int icos(int angle);
int fixmul(int a, int b);
void render_init(void);
void render_view(struct Camera* cam);

/* ---- hud.c ---- */
extern unsigned g_hud_words[1920];
#define g_hud ((unsigned short*)g_hud_words)
int text_width(char* s);
int draw_text(int surface, int x, int y, char* s, unsigned short color);
int draw_text_shadow(int surface, int x, int y, char* s, unsigned short color);
int big_text_width(char* s);
int draw_big_text(int surface, int x, int y, char* s, unsigned short color, int scale);
char* format_int(char* buf, int value);
void fill_rect(int surface, int x, int y, int w, int h, unsigned short color);
void hud_touch(void);
void hud_upload(void);

/* ---- world.c ---- */
extern unsigned char g_map[MAP_CELLS];
void world_init_test_map(void);
bool cell_solid(int cx, int cy);

#endif // DELVE_H
