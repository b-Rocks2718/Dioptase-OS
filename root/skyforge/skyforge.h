#ifndef SKYFORGE_H
#define SKYFORGE_H

/*
 * SKYFORGE — 3D Voxel-Space Canyon Flight, Grand Prix & World Sculptor
 * Created and signed by gemini-4-argon for Dioptase-OS.
 *
 * Shared constants, fixed-point conventions, world and entity records, and
 * cross-module interfaces. World coordinates use 16.16 fixed point where one
 * terrain cell equals 65536 (1.0). Angles use 10-bit turns (1024 units per
 * full circle: 0 is +X east, 256 is +Y south, 512 is -X west, 768 is -Y north).
 */

#ifdef SKYFORGE_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#else
#include "../crt/stdbool.h"
#include "../crt/stddef.h"
#endif

/* ---- Fixed-point and angle units ---- */
#define FIX_SHIFT 16
#define FIX_ONE 65536
#define FIX_HALF 32768
#define ANGLE_COUNT 1024
#define ANGLE_MASK 1023
#define ANGLE_QUARTER 256
#define ANGLE_HALF 512

/* ---- Display and TLB-friendly backbuffer layout ----
 * The 320x240 pixel framebuffer (docs/mem_map.md) displays at 2x (640x480)
 * when pixel scale is 0. User mode maps the first PIXEL_FB_USER_PIXELS
 * (75,776 pixels, rows 0..235; docs/syscalls.md).
 *
 * Rows 0..191 hold the 320x192 3D viewport, rendered as 160 double-pixel
 * columns (each horizontal pair is one 32-bit word `c | (c << 16)`).
 * To keep column walks inside the 16-entry hardware TLB (docs/ISA.md), the
 * backbuffer is divided into 20 column-groups of 8 double-columns x 192 rows
 * (1,536 words = 6,144 bytes = 1.5 pages per group). Stepping one row up or
 * down within a column is a uniform 32-byte (8-word) stride.
 *
 * Rows 192..231 hold the 320x40 graphical flight deck and topographic radar.
 * Rows 232..239 are covered by tile row 29 of the tile framebuffer at scale 1.
 */
#define FB_W 320
#define VIEW_W 320
#define VIEW_H 192
#define VIEW_COLS 160
#define VIEW_GROUPS 20
#define GROUP_COLS 8
#define GROUP_ROW_BYTES 32
#define GROUP_WORDS 1536
#define VIEW_WORDS 30720

#define HUD_Y 192
#define HUD_W 320
#define HUD_H 40
#define HUD_PIXELS 12800
#define HUD_WORDS 6400

/* Compute the word index in g_view_words for double-column cx (0..159) and
 * scanline y (0..191) using shifts and adds only (zero multiplications). */
#define VIEW_WORD_INDEX(cx, y) \
  ((((cx) >> 3) << 10) + (((cx) >> 3) << 9) + ((y) << 3) + ((cx) & 7))

/* ---- Tile layer and hardware sprites (docs/mem_map.md) ---- */
#define TILE_SCALE_HUD 1
#define TEXT_COLS 40
#define TEXT_ROWS 30
#define TEXT_BORDER_ROW 29
#define TILE_FB_STRIDE 80
#define TILEMAP_SHORTS_PER_TILE 64
#define TILE_CLEAR 0
#define TILE_BLACK 1
#define TILE_SOLID 2
#define TILE_BORDER 3
#define TILE_BAR_BASE 8

#define SPRITE_SHORTS 1024
#define SPRITE_OFFSCREEN 1000
#define TRANSPARENT_12BIT 0xF000

#define SPRITE_RETICLE 0
#define SPRITE_LOCK_BOX 1
#define SPRITE_WAYPOINT 2

/* ---- Voxel terrain grid and raymarching ---- */
#define MAP_SHIFT 7
#define MAP_SIZE 128
#define MAP_MASK 127
#define MAP_CELLS 16384

#define MAX_HEIGHT 63
#define WATER_LEVEL 8
#define VOXEL_STEPS 48
#define OCCL_BUCKETS 16
#define OCCL_BUFFER_SIZE 2560

/* Biomes / Campaign Sectors */
#define BIOME_COUNT 5
#define BIOME_EMERALD 0
#define BIOME_OBSIDIAN 1
#define BIOME_CRYO 2
#define BIOME_CYBER 3
#define BIOME_LEVIATHAN 4

#define MAX_SECTORS 5

/* ---- Game modes and state machine ---- */
#define MODE_CAMPAIGN 0
#define MODE_RACE 1
#define MODE_FORGE 2

#define STATE_TITLE 0
#define STATE_PLAY 1
#define STATE_PAUSE 2
#define STATE_HELP 3
#define STATE_SECTOR_CLEAR 4
#define STATE_VICTORY 5
#define STATE_GAMEOVER 6

/* ---- Terrain Forge sculptor tools ---- */
#define TOOL_RAISE 0
#define TOOL_LOWER 1
#define TOOL_SMOOTH 2
#define TOOL_CRATER 3
#define TOOL_CRYSTAL 4
#define TOOL_LAVA 5
#define TOOL_RING 6
#define TOOL_COUNT 7

/* ---- World entities, projectiles, and particles ---- */
#define ENT_NONE 0
#define ENT_RING 1
#define ENT_GATE 2
#define ENT_ARCH 3
#define ENT_SPIRE 4
#define ENT_SCOUT 5
#define ENT_INTERCEPTOR 6
#define ENT_TURRET 7
#define ENT_STEALTH 8
#define ENT_BOSS 9
#define ENT_BEACON 10
#define ENT_RIVAL 11

#define MAX_ENTITIES 64
#define MAX_SHOTS 32
#define MAX_PARTICLES 64
#define RACE_GATE_COUNT 24

#define SHOT_PLAYER_PLASMA 1
#define SHOT_PLAYER_SEISMIC 2
#define SHOT_ENEMY_LASER 3
#define SHOT_BOSS_ORB 4

/* One 3D world object (gate, ring, arch, enemy fighter, turret, or boss). */
struct Entity {
  int active;
  int kind;
  int x;          /* 16.16 world X */
  int y;          /* 16.16 world Y */
  int z;          /* 16.16 altitude (height * FIX_ONE) */
  int vx;         /* 16.16 velocity per tick */
  int vy;
  int vz;
  int angle;      /* 10-bit heading */
  int hp;
  int max_hp;
  int cooldown;
  int phase;      /* animation phase or gate sequence index */
  int flash;      /* hit-flash ticks remaining */
  int shield_gen; /* for boss: active shield generators bitmask */
};

/* One in-flight plasma bolt, enemy laser, or seismic torpedo. */
struct Shot {
  int active;
  int kind;
  int x;
  int y;
  int z;
  int vx;
  int vy;
  int vz;
  int life;
  int color;
};

/* One 3D ballistic spark, debris fragment, or thruster plume particle. */
struct Particle {
  int active;
  int x;
  int y;
  int z;
  int vx;
  int vy;
  int vz;
  int life;
  unsigned short color;
};

/* Player starfighter flight, combat, and Terrain Forge cursor state. */
struct Player {
  int x;              /* 16.16 world X */
  int y;              /* 16.16 world Y */
  int z;              /* 16.16 altitude */
  int angle;          /* 10-bit yaw heading */
  int pitch;          /* -48..+48 horizon pitch offset in scanlines */
  int roll;           /* -32..+32 bank angle */
  int barrel_spin;    /* 0..1024 active 360-degree barrel-roll spin */
  int barrel_cooldown;
  int speed;          /* 16.16 forward speed per tick */
  int hp;
  int max_hp;
  int energy;         /* 0..100 afterburner / barrel-roll energy */
  int weapon_level;   /* 1 = twin, 2 = quad, 3 = hyper-spread */
  int bombs;          /* seismic nova torpedo count */
  int fire_cooldown;
  int bomb_cooldown;
  int hurt_flash;
  int boost_active;
  int shake;
  int score;
  int combo;
  int combo_timer;
  int kills;
  int rings_passed;
  int beacons_saved;
  int next_gate;      /* Grand Prix next gate index (0..RACE_GATE_COUNT-1) */
  int lap;
  int race_ticks;
  int best_race_ticks;
  int aim_x;          /* screen aiming reticle X (-96..+96 from center) */
  int aim_y;          /* screen aiming reticle Y (-64..+64 from center) */
  int lock_target;    /* entity index currently locked on, or -1 */
  int cockpit_view;   /* 0 = 3rd-person Arwing, 1 = 1st-person cockpit */
  int forge_tool;     /* TOOL_* in Terrain Forge mode */
  int forge_radius;   /* brush radius in cells (2..8) */
  int forge_cursor_x; /* 0..127 cell X under 3D cursor */
  int forge_cursor_y; /* 0..127 cell Y under 3D cursor */
};

/* One tick of flight or sculptor controls produced by keyboard, mouse, or AI. */
struct FlightControls {
  int turn;           /* yaw input: -1 left, 0 neutral, +1 right */
  int pitch;          /* pitch input: -1 dive, 0 neutral, +1 climb */
  int throttle;       /* +1 boost, -1 brake, 0 cruise */
  int fire_primary;   /* 1 = fire twin plasma cannons */
  int fire_seismic;   /* 1 = launch seismic crater bomb */
  int barrel_roll;    /* 1 = trigger deflecting barrel roll */
  int sculpt_apply;   /* 1 = apply active Terrain Forge tool */
  int sculpt_erase;   /* 1 = inverse/lower Terrain Forge tool */
};

/* Camera pose passed to the 3D voxel and object renderer each frame. */
struct Camera {
  int x;
  int y;
  int z;
  int angle;
  int pitch;
  int roll;
  int biome;
};

/* ---- Sound effects and music IDs (audio.c) ---- */
#define SFX_PLASMA 1
#define SFX_SEISMIC_LAUNCH 2
#define SFX_CRATER_BOOM 3
#define SFX_RING_PASS 4
#define SFX_BARREL_ROLL 5
#define SFX_ENEMY_HIT 6
#define SFX_ENEMY_EXPLODE 7
#define SFX_PLAYER_HURT 8
#define SFX_BEACON_PICKUP 9
#define SFX_SCULPT_HUM 10
#define SFX_SECTOR_CLEAR 11
#define SFX_BOSS_ALARM 12

#define SONG_NONE 0
#define SONG_TITLE 1
#define SONG_EMERALD 2
#define SONG_CYBER 3
#define SONG_BOSS 4
#define SONG_FORGE 5

/* ---- Core affinity assignments (docs/syscalls.md, pin_core) ---- */
#define CORE_AUDIO 0
#define CORE_GAME 1
#define CORE_WORKER0 2
#define CORE_WORKER1 3

/* ---- Shared globals ---- */
extern unsigned short g_terrain[16384];
extern struct Entity g_entities[64];
extern struct Shot g_shots[32];
extern struct Particle g_particles[64];
extern struct Player g_player;
extern int g_sector;
extern int g_biome;
extern int g_mode;
extern int g_state;
extern unsigned g_frame;
extern unsigned g_seed;
extern bool g_autopilot;
extern bool g_demo_mode;
extern char g_banner_msg[40];
extern int g_banner_timer;

/* Display mappings and backbuffer (main.c / hud.c) */
extern volatile short* g_pixel_fb;
extern volatile short* g_tile_fb;
extern volatile short* g_tilemap;
extern volatile short* g_spritemap;
extern unsigned g_view_words[30720];
extern unsigned g_hud_words[6400];

/* ---- gfx.s (user-mode assembly fast paths) ---- */
/* Multiply two 16-bit unsigned integers in user mode without hardware mul. */
unsigned umul16(unsigned a, unsigned b);
/* Unsigned 32-bit integer division returning quotient. */
unsigned udiv32(unsigned n, unsigned d);
/* Copy `words` 32-bit words from `src` to `dst` (both 4-byte aligned). */
void copy_words(void* dst, void* src, unsigned words);
/* Fill `words` 32-bit words at `dst` with `value`. */
void fill_words(void* dst, unsigned value, unsigned words);
/* Blit column-groups [g0, g1) from the chunked backbuffer to the 320x192 pixel framebuffer. */
void blit_strip_groups(volatile short* fb, unsigned* view, int g0, int g1);

/* ---- math_lut.c ---- */
extern int sin_lut[1024];
extern unsigned short biome_palettes[1280];
extern unsigned short sky_gradients[160];
extern unsigned char font_8x8[768];
/* Return 16.16 sine of 10-bit angle `a`. */
int isin(int a);
/* Return 16.16 cosine of 10-bit angle `a`. */
int icos(int a);
/* Multiply two signed 16.16 fixed-point numbers. */
int fixmul(int a, int b);
/* Return fast octant approximation of 2D distance |(dx, dy)|. */
int approx_dist2d(int dx, int dy);
/* Expand a 12-bit 0x0BGR color into a duplicated 32-bit pixel-pair word. */
unsigned pack_pair(unsigned short c);
/* Blend or darken a 12-bit 0x0BGR color toward fog color by level 0..7. */
unsigned short shade_color12(unsigned short col, unsigned short fog, int fog_level);
/* Initialize lookup tables and palettes. */
void math_init(void);

/* ---- world.c ---- */
/* Seed the deterministic xorshift PRNG. */
void rng_seed(unsigned s);
/* Return the next 32-bit pseudo-random number. */
unsigned rng_next(void);
/* Return a pseudo-random integer in [0, bound). */
int rng_range(int bound);
/* Sample the terrain height (0..63) at cell (cx, cy) with wrapping. */
int terrain_height_cell(int cx, int cy);
/* Sample the 16.16 terrain altitude at 16.16 world position (x, y). */
int terrain_height_at(int x, int y);
/* Recompute directional sun slope and biome color indices over a cell rectangle. */
void terrain_relight_rect(int x0, int y0, int x1, int y1);
/* Carve a 3D crater or raise a peak at cell (cx, cy) and relight slopes. */
void terrain_deform(int cx, int cy, int radius, int delta_h, int paint_kind);
/* Apply the active Terrain Forge tool at (cx, cy). */
void forge_apply_tool(int cx, int cy, int tool, int radius, bool inverse);
/* Generate a procedural 3D world, river canyon, and entities for sector/mode. */
void world_generate(int sector, int mode, unsigned seed);
/* Advance player, entities, projectiles, particles, and collisions by one 60 Hz tick. */
void world_tick(struct FlightControls* ctrl);
/* Format a status banner message shown near the top of the viewport. */
void set_status_banner(char* msg, int duration_ticks);
/* Count remaining hostile targets in the current campaign sector. */
int world_enemies_remaining(void);

/* ---- autopilot.c ---- */
/* Compute one tick of autonomous AI Ace flight and combat controls. */
void autopilot_decide(struct FlightControls* ctrl);

/* ---- render.c ---- */
/* Initialize renderer tables and perspective projection buffers. */
void render_init(void);
/* Render the 3D voxel terrain, sky, 3D objects, and player starfighter into g_view_words. */
void render_scene(struct Camera* cam);
/* Compute a deterministic 32-bit FNV-1a hash of the rendered 3D backbuffer. */
unsigned render_view_checksum(void);

/* ---- hud.c ---- */
/* Initialize tilemap glyphs, banners, and hardware sprites. */
void hud_init(void);
/* Render the 320x40 graphical cockpit panel and upload to the pixel framebuffer. */
void hud_draw_panel(void);
/* Update hardware sprite positions for reticle, target lock box, and waypoint. */
void hud_update_sprites(void);
/* Hide all hardware sprites before returning to the shell. */
void hud_hide_sprites(void);
/* Begin immediate-mode tile text frame. */
void text_begin(void);
/* Draw a string at tile (col, row) in RGB332 color. */
void tile_text(int col, int row, char* s, int color);
/* Draw a centered string on tile row `row` in RGB332 color. */
void tile_text_centered(int row, char* s, int color);
/* Draw all tile overlays for the current game state and flush changed cells. */
void hud_draw_overlays(int menu_cursor);
/* Flush desired tile entries to the tile framebuffer. */
void text_end(void);
/* Format signed integer `v` into `buf` and return `buf`. */
char* fmt_int(char* buf, int v);

/* ---- audio.c ---- */
/* Fork the background 25 kHz PCM synthesizer process unless silent mode is set. */
void audio_start(bool silent);
/* Stop and reap the background synthesizer process. */
void audio_stop(void);
/* Trigger sound effect `sfx_id` (SFX_*). */
void sfx_trigger(int sfx_id);
/* Switch background music to `song_id` (SONG_*). */
void audio_set_song(int song_id);
/* Send periodic keepalive heartbeat to the synthesizer process. */
void audio_heartbeat(void);
/* Toggle audio mute state and return the new mute status. */
bool audio_toggle_mute(void);
/* Synthesize one chunk of PCM directly into `out_pcm` (for headless self-test). */
void audio_synth_test_chunk(short* out_pcm, int samples);

/* ---- save.c ---- */
#define HIGH_SCORE_SLOTS 5
extern int g_high_scores[5];
extern int g_best_lap_ticks;
/* Load persistent high scores and best race time from /skyforge/scores.sf. */
void scores_load(void);
/* Record a new campaign score or race time and persist to /skyforge/scores.sf. */
void scores_record(int score, int race_ticks);
/* Save the current 128x128 3D terrain and biome to /skyforge/world.sf. */
bool world_save_file(char* path);
/* Load a 128x128 3D terrain and biome from /skyforge/world.sf. */
bool world_load_file(char* path);

#endif /* SKYFORGE_H */
