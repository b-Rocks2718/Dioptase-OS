#ifndef SKYFORGE_H
#define SKYFORGE_H

/*
 * SKYFORGE — 3D Voxel-Space Canyon Flight, Grand Prix & World Sculptor
 * Created and signed by gemini-4-argon for Dioptase-OS.
 *
 * Full 320x192 single-pixel 3D viewport with 64x64 textured terrain/cliffs,
 * 64x64 shaded pixel-art billboards, 5 banked 80x48 player starfighter frames,
 * and an 8-channel 25 kHz mu-law sample-playback tracker with reverb.
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

/* ---- Display and TLB-shaped single-pixel backbuffer layout ----
 * The 320x192 3D view is kept in two halves of 96 rows split at the horizon.
 * Each half consists of 40 chunks of 8x96 pixels (one per 8-pixel column
 * group), row-major with 16-byte rows, so pixel (x, y) is the short at:
 *   (y >= 96) * HALF_SHORTS + (x >> 3) * CHUNK_SHORTS + (y % 96) * 8 + (x & 7).
 * Stepping down a column is an 8-short (16-byte) stride that jumps HALF_SKIP
 * bytes at row 96, so each column touches at most two 4 KiB TLB pages.
 */
#define FB_W 320
#define VIEW_W 320
#define VIEW_H 192
#define VIEW_HALF_H 96
#define VIEW_GROUPS 40
#define HALF_SHORTS 30720
#define CHUNK_SHORTS 768
#define VIEW_WORDS 30720
#define HALF_LEFT(y) ((y) < VIEW_HALF_H ? VIEW_HALF_H - (y) : VIEW_H - (y))

#define HUD_Y 192
#define HUD_W 320
#define HUD_H 40
#define HUD_PIXELS 12800
#define HUD_WORDS 6400

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

/* ---- Voxel terrain grid, textures, and sprites ---- */
#define MAP_SHIFT 7
#define MAP_SIZE 128
#define MAP_MASK 127
#define MAP_CELLS 16384

#define MAX_HEIGHT 63
#define WATER_LEVEL 8
#define VOXEL_STEPS 48
#define OCCL_BUCKETS 16

/* 8 64x64 procedural terrain textures (assets.s order) */
#define TEX_SIZE 64
#define TEX_BYTES 4096
#define TEX_WATER 0
#define TEX_SAND 1
#define TEX_GRASS 2
#define TEX_ROCK 3
#define TEX_CRYSTAL 4
#define TEX_SNOW 5
#define TEX_LAVA 6
#define TEX_CYBER 7

/* 28 64x64 billboard sprite frames (assets.s order) */
#define SPR_SIZE 64
#define SPR_BYTES 4096
#define SPR_RING 0          /* 4 frames: 0..3 */
#define SPR_GATE_ACTIVE 4   /* 2 frames: 4..5 */
#define SPR_GATE_NEXT 6     /* 1 frame */
#define SPR_ARCH 7          /* 1 frame */
#define SPR_GENERATOR 8     /* 2 frames: 8..9 */
#define SPR_PORTAL 10       /* 4 frames: 10..13 */
#define SPR_SCOUT 14        /* 2 frames: 14..15 */
#define SPR_INTERCEPTOR 16  /* 2 frames: 16..17 */
#define SPR_STEALTH 18      /* 2 frames: 18..19 */
#define SPR_TURRET 20       /* 1 frame */
#define SPR_BOSS 21         /* 2 frames: 21..22 */
#define SPR_ORB_PLAYER 23   /* 1 frame */
#define SPR_ORB_SEISMIC 24  /* 1 frame */
#define SPR_ORB_ENEMY 25    /* 1 frame */
#define SPR_EXPLODE 26      /* 2 frames: 26..27 */
#define SPR_COUNT 28

/* 5 banked 80x48 player starfighter frames (assets.s) */
#define SHIP_W 80
#define SHIP_H 48
#define SHIP_FRAMES 5

#define LIGHT_LEVELS 16
#define LIGHT_FULL 12

/* Biomes / Campaign Sectors */
#define BIOME_COUNT 5
#define BIOME_EMERALD 0
#define BIOME_OBSIDIAN 1
#define BIOME_CRYO 2
#define BIOME_CYBER 3
#define BIOME_LEVIATHAN 4

#define MAX_SECTORS 5

/* ---- Structured 3-phase Campaign progression ----
 * Phase 0 (PHASE_TOWERS): Destroy the 3 Canyon Shield Towers & Squadrons.
 * Phase 1 (PHASE_GUARDIAN): Defeat the Sector Guardian / Leviathan Boss.
 * Phase 2 (PHASE_PORTAL): Fly through the Emerald Dioptase Warp Portal!
 */
#define PHASE_TOWERS 0
#define PHASE_GUARDIAN 1
#define PHASE_PORTAL 2

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
#define ENT_GENERATOR 4
#define ENT_SCOUT 5
#define ENT_INTERCEPTOR 6
#define ENT_TURRET 7
#define ENT_STEALTH 8
#define ENT_BOSS 9
#define ENT_PORTAL 10
#define ENT_RIVAL 11
#define ENT_EXPLOSION 12

#define MAX_ENTITIES 64
#define MAX_SHOTS 32
#define MAX_PARTICLES 64
#define RACE_GATE_COUNT 24

#define SHOT_PLAYER_PLASMA 1
#define SHOT_PLAYER_SEISMIC 2
#define SHOT_ENEMY_LASER 3
#define SHOT_BOSS_ORB 4

/* One 3D world object (ring, gate, arch, shield tower, fighter, boss, or portal). */
struct Entity {
  int active;
  int kind;
  int x;          /* 16.16 world X */
  int y;          /* 16.16 world Y */
  int z;          /* 16.16 altitude */
  int vx;
  int vy;
  int vz;
  int angle;
  int hp;
  int max_hp;
  int cooldown;
  int phase;
  int flash;
  int shield_gen;
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

/* One 3D ballistic spark or debris particle. */
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

/* Player starfighter flight, combat, and Terrain Forge hover-drone state. */
struct Player {
  int x;
  int y;
  int z;
  int angle;          /* 10-bit yaw heading */
  int turn_vel;       /* smoothed angular velocity */
  int pitch;          /* -36..+36 horizon pitch offset */
  int roll;           /* -32..+32 bank angle */
  int barrel_spin;    /* 0..1024 active 360-degree barrel-roll spin */
  int barrel_cooldown;
  int speed;          /* 16.16 forward speed */
  int hp;
  int max_hp;
  int energy;         /* 0..100 afterburner / barrel-roll energy */
  int weapon_level;   /* 1 = twin, 2 = quad, 3 = hyper-spread */
  int bombs;
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
  int towers_left;    /* remaining Canyon Shield Towers in Phase 0 */
  int sector_phase;   /* PHASE_TOWERS, PHASE_GUARDIAN, or PHASE_PORTAL */
  int next_gate;      /* Grand Prix next gate index */
  int lap;
  int race_ticks;
  int best_race_ticks;
  int aim_x;          /* screen aiming reticle X (-110..+110 from center) */
  int aim_y;          /* screen aiming reticle Y (-68..+68 from center) */
  int lock_target;    /* locked entity index, or -1 */
  int nav_target;     /* primary mission objective entity index, or -1 */
  int nav_bearing;    /* signed angle delta to nav_target (-512..+511) */
  int cockpit_view;   /* 0 = 3rd-person Arwing, 1 = 1st-person cockpit */
  int forge_tool;
  int forge_radius;
  int forge_cursor_x;
  int forge_cursor_y;
};

/* One tick of flight or hover-drone sculptor controls. */
struct FlightControls {
  int turn;           /* yaw input: -16..+16 */
  int pitch;          /* pitch input: -16..+16 */
  int strafe;         /* hover-drone lateral strafe: -1, 0, +1 */
  int throttle;       /* +1 boost/forward, -1 brake/reverse, 0 cruise */
  int fire_primary;
  int fire_seismic;
  int barrel_roll;
  int sculpt_apply;
  int sculpt_erase;
};

/* Camera pose passed to the 3D voxel and billboard renderer each frame. */
struct Camera {
  int x;
  int y;
  int z;
  int angle;
  int pitch;
  int roll;
  int biome;
};

/* Reverb delay line state read/written by reverb_mix in gfx.s. */
struct Reverb {
  int* comb1;
  int* comb1_end;
  int* comb1_at;
  int* comb2;
  int* comb2_end;
  int* comb2_at;
  int* allpass;
  int* allpass_end;
  int* allpass_at;
  int lp1;
  int lp2;
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
#define SONG_VICTORY 6

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

extern volatile short* g_pixel_fb;
extern volatile short* g_tile_fb;
extern volatile short* g_tilemap;
extern volatile short* g_spritemap;
extern unsigned g_view_words[30720];
#define g_view ((unsigned short*)g_view_words)
extern unsigned g_hud_words[6400];

/* ---- assets.s ---- */
extern unsigned short shade_tables[4096];
extern int sin_lut[1024];
extern unsigned char terrain_textures[32768];
extern unsigned char sprite_texels[114688];
extern unsigned char sprite_spans[3584];
extern unsigned short ship_images[19200];
extern unsigned char ship_spans[800];

/* ---- gfx.s ---- */
unsigned umul16(unsigned a, unsigned b);
unsigned udiv32(unsigned n, unsigned d);
void copy_words(void* dst, void* src, unsigned words);
void fill_words(void* dst, unsigned value, unsigned words);
void blit_strip_groups(volatile short* fb, unsigned* view, int g0, int g1);
void draw_voxel_span(unsigned short* dst, int count, unsigned char* texcol,
                     int texpos, int texstep, unsigned short* shade, int half_left);
void draw_sprite_col(unsigned short* dst, int count, unsigned char* texcol,
                     int texpos, int texstep, unsigned short* shade, int half_left);
void draw_image_col(unsigned short* dst, int count, unsigned short* src, int half_left);
void mix_add(int* acc, int count, short* table, unsigned phase, unsigned inc);
unsigned mix_sample(int* acc, int count, unsigned char* data, unsigned pos, unsigned step,
                    unsigned end, unsigned loop_len, short* vol_row);
void reverb_mix(int* acc, short* out, int count, struct Reverb* reverb);

/* ---- math_lut.c ---- */
extern unsigned short biome_palettes[1280];
extern unsigned short sky_gradients[160];
extern unsigned char font_8x8[768];
int isin(int a);
int icos(int a);
int fixmul(int a, int b);
int approx_dist2d(int dx, int dy);
unsigned pack_pair(unsigned short c);
unsigned short shade_color12(unsigned short col, unsigned short fog, int fog_level);
void math_init(void);

/* ---- world.c ---- */
void rng_seed(unsigned s);
unsigned rng_next(void);
int rng_range(int bound);
int terrain_height_cell(int cx, int cy);
int terrain_height_at(int x, int y);
void terrain_relight_rect(int x0, int y0, int x1, int y1);
void terrain_deform(int cx, int cy, int radius, int delta_h, int paint_kind);
void forge_apply_tool(int cx, int cy, int tool, int radius, bool inverse);
void world_generate(int sector, int mode, unsigned seed);
void world_tick(struct FlightControls* ctrl);
void set_status_banner(char* msg, int duration_ticks);
int world_enemies_remaining(void);
int canyon_center_at(int cy);

/* ---- autopilot.c ---- */
void autopilot_decide(struct FlightControls* ctrl);

/* ---- render.c ---- */
void render_init(void);
void render_scene(struct Camera* cam);
unsigned render_view_checksum(void);

/* ---- hud.c ---- */
void hud_init(void);
void hud_draw_panel(void);
void hud_update_sprites(void);
void hud_hide_sprites(void);
void text_begin(void);
void tile_text(int col, int row, char* s, int color);
void tile_text_centered(int row, char* s, int color);
void hud_draw_overlays(int menu_cursor);
void text_end(void);
char* fmt_int(char* buf, int v);

/* ---- audio.c ---- */
void audio_start(bool silent);
void audio_stop(void);
void sfx_trigger(int sfx_id);
void audio_set_song(int song_id);
void audio_heartbeat(void);
bool audio_toggle_mute(void);
void audio_synth_test_chunk(short* out_pcm, int samples);

/* ---- save.c ---- */
#define HIGH_SCORE_SLOTS 5
extern int g_high_scores[5];
extern int g_best_lap_ticks;
void scores_load(void);
void scores_record(int score, int race_ticks);
bool world_save_file(char* path);
bool world_load_file(char* path);

#endif /* SKYFORGE_H */
