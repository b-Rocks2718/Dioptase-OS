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
 * (value - 1). Values from CELL_DOOR_BASE are doors, indexing g_doors. */
#define CELL_EMPTY 0
#define CELL_DOOR_BASE 64

/* Doors are thin sliding panels on a cell's center plane. `axis_x` means the
 * panel lies on the plane x = cx + 0.5 (the doorway runs east-west). `open`
 * is how far the panel has slid aside (16.16, FIX_ONE = fully open). */
struct Door {
  int cx;
  int cy;
  int axis_x;
  int locked;
  int open;
  int opening;
};
#define MAX_DOORS 40
#define DOOR_PASSABLE 0xC000

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
#define TEX_CRACKED 8          /* breakable wall hiding a vault */

/* ---- Billboard sprites (assets.s order) ---- */
#define SPR_SIZE 32
#define SPR_BYTES 1024
#define SPR_SLIME 0        /* 4 frames */
#define SPR_BAT 4          /* 4 frames */
#define SPR_EYE 8          /* 5 frames: 0-2 look, 3 blink, 4 charge */
#define SPR_WRAITH 13      /* 4 frames */
#define SPR_CRYSTAL 17     /* 4 frames */
#define SPR_POTION_HP 21
#define SPR_POTION_MP 22
#define SPR_KEY 23
#define SPR_PORTAL 24      /* 4 frames */
#define SPR_ORB_SPARK 28
#define SPR_ORB_FIRE 29
#define SPR_ORB_ENEMY 30
#define SPR_PUFF_FIRE 31   /* 4 frames */
#define SPR_PUFF_GEM 35    /* 4 frames */
#define SPR_PUFF_SPARK 39  /* 4 frames */
#define SPR_PUFF_PINK 43   /* 4 frames */
#define SPR_TORCH 47       /* 4 frames */
#define SPR_WARDEN 51      /* 3 frames: 0-1 idle, 2 charging */
#define SPR_BONES 54
#define SPR_MUSHROOMS 55
#define SPR_COUNT 56

/* One sprite to draw this frame. Positions are world 16.16; `z` is the
 * height of the sprite's bottom edge above the floor and `size` its world
 * height (and width). */
struct Billboard {
  int x;
  int y;
  int z;
  int size;
  int frame;
  int light;       /* extra light levels (negative darkens) */
  int flags;
  int depth;       /* filled in by the renderer */
};
#define BB_FULLBRIGHT 1    /* ignore distance fog (glowing things) */
#define BB_FLASH 2         /* draw blown-out white (damage flash) */
#define MAX_BILLBOARDS 64

/* ---- World entities ---- */
#define MONSTER_KINDS 5
#define MON_SLIME 0
#define MON_BAT 1
#define MON_EYE 2
#define MON_WRAITH 3
#define MON_WARDEN 4      /* final-depth boss; carries the rune key */

struct Monster {
  int alive;
  int kind;
  int hp;
  int cx;          /* cell it occupies (or is moving into) */
  int cy;
  int x;           /* drawn position, 16.16 */
  int y;
  int from_x;
  int from_y;
  int move_t;
  int move_len;    /* 0 when not moving */
  int cooldown;
  int attack_t;
  int flash;
  int anim;
  int awake;
};
#define MAX_MONSTERS 24

#define ITEM_CRYSTAL 0
#define ITEM_POTION_HP 1
#define ITEM_POTION_MP 2
#define ITEM_KEY 3
#define ITEM_PORTAL 4
#define ITEM_HEART 5

struct Item {
  int active;
  int type;
  int cx;
  int cy;
  int phase;
};
#define MAX_ITEMS 40

#define SHOT_SPARK 0
#define SHOT_FIRE 1
#define SHOT_ENEMY 2

struct Projectile {
  int active;
  int kind;
  int x;
  int y;
  int vx;          /* per tick, 16.16 */
  int vy;
  int from_player;
  int life;
};
#define MAX_SHOTS 24

#define EFFECT_FIRE_PUFF 0
#define EFFECT_GEM_PUFF 1
#define EFFECT_SPARK_PUFF 2
#define EFFECT_PINK_PUFF 3

/* Spark particle: a single bright pixel with simple ballistics. */
struct Particle {
  int x;
  int y;
  int z;           /* height above the floor, 16.16 */
  int vx;          /* per tick, 16.16 */
  int vy;
  int vz;
  int life;        /* ticks left, 0 = free slot */
  int color;       /* 12-bit 0x0BGR */
};
#define MAX_PARTICLES 40

/* Wall torch: a flame billboard just in front of a wall face. */
struct Torch {
  int x;
  int y;
  int phase;
};
#define MAX_TORCHES 16

/* Floor clutter: bones and glowing mushrooms (decoration only). */
struct Prop {
  int cx;
  int cy;
  int kind;        /* 0 bones, 1 mushrooms */
};
#define MAX_PROPS 10

struct Effect {
  int active;
  int kind;
  int x;
  int y;
  int z;
  int t;
};
#define MAX_EFFECTS 24

/* Point light for this frame: within `radius` (16.16) of (x, y) it adds one
 * light level per half cell of remaining radius. */
struct Light {
  int x;
  int y;
  int radius;
};
#define MAX_LIGHTS 24

#define MAX_HP 100
#define MAX_MP 100
#define FINAL_DEPTH 8

/* The player is a square of half-width PLAYER_RADIUS (16.16 cells) for
 * collisions; corridors are one cell wide. Biting monsters reach the player
 * within MONSTER_REACH of their center. */
#define PLAYER_RADIUS 0x4400
#define MONSTER_REACH 0x14000

struct PlayerStats {
  int hp;
  int max_hp;
  int mp;
  int crystals;
  int keys;
  int depth;
  int score;
  int kills;
  int exit_reached;   /* 1 portal, 2 the Heart (victory) */
  int shake;          /* camera shake ticks */
  int hurt;           /* red flash ticks */
  /* powers bought by crystal milestones (see world.c crystal_milestone) */
  int spark_damage;
  int fire_damage;
  int splash_damage;
  int regen_ticks;    /* ticks per point of mana regained */
};

/* Sound effects (audio.c). */
#define SFX_SPARK 1
#define SFX_FIRE 2
#define SFX_EXPLODE 3
#define SFX_FIZZLE 4
#define SFX_MONSTER_HURT 5
#define SFX_MONSTER_DIE 6
#define SFX_PLAYER_HURT 7
#define SFX_CRYSTAL 8
#define SFX_POTION 9
#define SFX_KEY 10
#define SFX_DOOR 11
#define SFX_STEP 12
#define SFX_GROWL 13
#define SFX_ENEMY_SHOOT 14
#define SFX_LOCKED 15
#define SFX_PORTAL 16
#define SFX_NOMANA 17
#define SFX_DEATH 18
#define SFX_BUMP 19
#define SFX_DRIP 20
#define SFX_CRUMBLE 21

/* ---- Camera ---- */
struct Camera {
  int x;
  int y;
  int angle;
};

/* Per-row floor casting state for draw_flats_asm() in gfx.s (field order is
 * ABI: 24 bytes per record). */
struct FlatRow {
  int u;
  int v;
  int du;
  int dv;
  int level;       /* the row's light level before point lights */
  int unused;
};

/* A point light as draw_flats_asm() reads it (x, y, radius words). */
struct FlatLight {
  int x;
  int y;
  int radius;
};

/* ---- gfx.s ---- */
void blit_view(volatile short* dst, unsigned short* src);
void copy_words(void* dst, void* src, unsigned words);
void draw_wall(unsigned short* dst, int count, unsigned char* texcol,
               int texpos, int texstep, unsigned short* shade);
void fill_rows(unsigned short* dst, int count, unsigned short* colors, int advance);
unsigned umul16(unsigned a, unsigned b);
unsigned udiv32(unsigned n, unsigned d);
void draw_sprite_col(unsigned short* dst, int count, unsigned char* texcol,
                     int texpos, int texstep, unsigned short* shade);
void dim_view(unsigned short* view);
void mix_add(int* acc, int count, short* table, unsigned phase, unsigned inc);
void tint_view(unsigned short* view, int and_mask, int or_mask);
void draw_flats_asm(struct FlatRow* rows, unsigned short* view,
                    unsigned char* floor_tex, unsigned char* ceil_tex,
                    struct FlatLight* lights, int light_count, int* skip,
                    int columns);

/* ---- assets.s ---- */
extern unsigned short shade_tables[4096];
extern unsigned char wall_textures[36864];
extern unsigned char flat_textures[16384];
#define FLAT_FLOOR_STONE 0
#define FLAT_CEILING_BEAMS 1
#define FLAT_FLOOR_MOSS 2
#define FLAT_CEILING_ROCK 3
extern int sin_table[4096];
extern unsigned char sprite_texels[57344];
extern unsigned char sprite_spans[3584];
extern unsigned note_incs[128];               /* phase step per MIDI note at 25 kHz */
extern int song_table[16];                    /* per song: row samples, rows, loop, offset */
extern unsigned char song_data[1408];         /* 4 bytes (lead, arp, bass, drums) per row */
extern unsigned short overlay_staff[2048];   /* two 32x32 sprites, 12-bit */
extern unsigned short overlay_cursor[1024];
extern unsigned short overlay_staff_glow[1024]; /* top staff sprite while casting */
extern unsigned char font_small[320];
extern unsigned char font_big[768];
extern unsigned char font_big_spans[192];

/* ---- main.c ---- */
extern volatile short* g_tilemap;
extern volatile short* g_tile_fb;
extern volatile short* g_spritemap;
/* The back buffer for the 3D view. It points at g_view_words, or at the
 * shared mapping when parallel rendering is running (parallel.c). Buffers
 * copied with word loads/stores are declared as word arrays because bcc only
 * 2-aligns short arrays. */
extern unsigned g_view_words[7680];
extern unsigned short* g_view;
extern unsigned g_frame;            /* VGA frames since start (60 Hz) */

/* ---- render.c ---- */
int isin(int angle);
int icos(int angle);
int fixmul(int a, int b);
void render_init(void);
void render_view(struct Camera* cam);
void set_light_offset(int levels);
int get_light_offset(void);
void render_view_strip(struct Camera* cam, int g0, int g1);
extern struct Billboard g_bills[MAX_BILLBOARDS];
extern int g_bill_count;
extern int g_debug_bills;
extern int g_debug_visible;
struct Billboard* add_billboard(int x, int y, int z, int size, int frame);

/* ---- hud.c ---- */
extern unsigned g_hud_words[1920];
#define g_hud ((unsigned short*)g_hud_words)
int text_width(char* s);
int draw_text(int surface, int x, int y, char* s, unsigned short color);
int draw_text_shadow(int surface, int x, int y, char* s, unsigned short color);
int big_text_width(char* s);
int draw_big_text(int surface, int x, int y, char* s, unsigned short color, int scale);
void draw_logo(int x, int y, char* s, int scale, int t);
char* format_int(char* buf, int value);
void fill_rect(int surface, int x, int y, int w, int h, unsigned short color);
void hud_touch(void);
void hud_upload(void);

/* ---- world.c ---- */
extern unsigned char g_map[MAP_CELLS];
extern unsigned char* g_seen;          /* explored cells; may live in shared memory */
extern struct Door g_doors[MAX_DOORS];
extern int g_door_count;
extern struct Monster g_monsters[MAX_MONSTERS];
extern struct Item g_items[MAX_ITEMS];
extern struct Projectile g_shots[MAX_SHOTS];
extern struct Light g_lights[MAX_LIGHTS];
extern int g_light_count;
extern struct PlayerStats g_stats;
extern int g_floor_flat;
extern int g_ceiling_flat;
void rng_seed(unsigned seed);
unsigned rng_next(void);
int rng_range(int n);
int approx_dist(int dx, int dy);
bool cell_solid(int cx, int cy);
struct Door* door_at(int cx, int cy);
bool door_open(int cx, int cy);
struct Monster* monster_at(int cx, int cy);
void world_init(void);
void world_generate(int depth, unsigned seed, int* sx, int* sy, int* sdir);
void world_tick(int pcx, int pcy, int px, int py, bool player_moved);
void world_draw(int frame, int cam_x, int cam_y);
void spawn_shot(int kind, int x, int y, int vx, int vy, bool from_player);
void spawn_effect(int kind, int x, int y, int z);
void spawn_burst(int x, int y, int z, int count, int color, int speed);
extern struct Particle g_particles[MAX_PARTICLES];
int world_monsters_left(void);
bool world_line_clear(int x0, int y0, int x1, int y1);
int world_flow(int cx, int cy);
bool world_boss(int* hp, int* max_hp);

/* ---- parallel.c ---- */
void parallel_start(void);
void parallel_render(struct Camera* cam);
void parallel_stop(void);
int parallel_workers(void);
void parallel_toggle(void);
void parallel_autotune_begin(void);

/* ---- autopilot.c ---- */
/* One tick of player controls: what a human holds on the keyboard, or what
 * the autopilot chooses. `turn` is in angle units for this tick (positive
 * turns right, clockwise on the map); `cast` is 0 or SHOT_SPARK + 1 /
 * SHOT_FIRE + 1 for a spell cast straight ahead. */
struct Controls {
  int forward;     /* 1 forward, -1 back */
  int strafe;      /* 1 right, -1 left */
  int turn;
  bool run;
  int cast;
};
#define TURN_MAX 44       /* fastest turn, angle units per tick (~230 deg/s) */
void autopilot_steer(int x, int y, int angle, bool has_key, int mp, struct Controls* c);
void autopilot_reset(void);

/* ---- main.c (game) ---- */
void player_hurt(int amount);
void show_message(char* text);

/* ---- audio.c ---- */
#define MUSIC_NONE 0
#define MUSIC_TITLE 1
#define MUSIC_DUNGEON 2
#define MUSIC_VICTORY 3
#define MUSIC_BOSS 4
void sfx(int id);
void audio_start(void);
void audio_stop(void);
void audio_music(int song);
void audio_heartbeat(void);
void audio_mute_sfx(bool on);

#endif // DELVE_H
