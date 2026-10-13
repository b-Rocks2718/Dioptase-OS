#ifndef REALM_H
#define REALM_H

#include "../crt/stdbool.h"
#include "../crt/stddef.h"

/*
 * Screen layout constants. The pixel framebuffer exposes rows 0..235 to
 * userland (PIXEL_FB_USER_PIXELS). Rows 0..207 form the scrolling battlefield
 * viewport; rows 208..235 host the static command console, resource readouts,
 * minimap radar, and action hotkeys.
 */
#define SCREEN_W 320
#define SCREEN_H 240
#define VIEW_H 208
#define HUD_H 28
#define HUD_Y 208
#define MAX_RENDER_Y 236

/*
 * World map geometry. The island comprises a 64x64 grid of 16x16 pixel terrain
 * tiles, spanning 1024x1024 world pixels. Sub-pixel physics uses 8.8 fixed-point
 * coordinates (256 units per pixel).
 */
#define MAP_SIZE 64
#define TILE_PX 16
#define WORLD_PX (MAP_SIZE * TILE_PX)
#define FIXED_SHIFT 8
#define FIXED_ONE 256
#define FIXED_HALF 128

/*
 * Color conversion helper for Dioptase's 12-bit 0x0BGR display format:
 * bits 3:0 Red, bits 7:4 Green, bits 11:8 Blue (0..15 each).
 */
#define COLOR_RGB(r, g, b) (((short)(r) & 0xF) | (((short)(g) & 0xF) << 4) | (((short)(b) & 0xF) << 8))
#define COLOR_TRANSPARENT ((short)0xF000)

/*
 * Bounded entity capacity limits to ensure static allocation without heap churn.
 */
#define MAX_VILLAGERS 24
#define MAX_SOLDIERS 32
#define MAX_ENEMIES 48
#define MAX_BUILDINGS 32
#define MAX_PROJECTILES 32
#define MAX_PARTICLES 48
#define MAX_FLOAT_TEXTS 16

/* Terrain classification for map tiles. */
enum TerrainType {
  TERRAIN_GRASS,
  TERRAIN_DIRT,
  TERRAIN_WATER,
  TERRAIN_PATH,
  TERRAIN_SHORE
};

/* Natural harvestable resource types on the world grid. */
enum ResourceType {
  RES_NONE,
  RES_TREE,
  RES_STONE,
  RES_GOLD
};

/* Building structures constructible by the kingdom. */
enum BuildingType {
  BLD_NONE,
  BLD_CASTLE,
  BLD_HOUSE,
  BLD_FARM,
  BLD_LUMBER_CAMP,
  BLD_BARRACKS,
  BLD_TOWER,
  BLD_WALL
};

/* Player and hostile unit classes. */
enum UnitType {
  UNIT_NONE,
  UNIT_VILLAGER,
  UNIT_SWORDSMAN,
  UNIT_ARCHER,
  UNIT_GOBLIN,
  UNIT_ORC,
  UNIT_SKELETON,
  UNIT_OGRE,
  UNIT_BOSS
};

/* Team allegiance. */
enum TeamId {
  TEAM_PLAYER,
  TEAM_ENEMY
};

/* Unit behavioral state. */
enum UnitState {
  STATE_IDLE,
  STATE_WALK,
  STATE_GATHER,
  STATE_RETURN,
  STATE_BUILD,
  STATE_ATTACK,
  STATE_HURT,
  STATE_DEAD
};

/* Day and night progression stages. */
enum DayPhase {
  PHASE_DAWN,
  PHASE_DAY,
  PHASE_DUSK,
  PHASE_NIGHT
};

/* High-level game flow screens. */
enum GameState {
  STATE_TITLE,
  STATE_PLAYING,
  STATE_PAUSED,
  STATE_GAMEOVER,
  STATE_VICTORY,
  STATE_HELP
};

/* Represents a single 16x16 world cell containing terrain and harvestable assets. */
struct MapCell {
  char terrain;
  char resource;
  short resource_hp;
  char building_id;
};

/* Represents a building structure placed in the realm. */
struct Building {
  char active;
  char type;
  char tile_x;
  char tile_y;
  char width;
  char height;
  short hp;
  short max_hp;
  short build_progress;
  short cooldown;
};

/* Represents a living player citizen, military recruit, or invading monster. */
struct Unit {
  char active;
  char type;
  char team;
  char state;
  int x;
  int y;
  int target_x;
  int target_y;
  char target_unit;
  char target_building;
  char target_tile_x;
  char target_tile_y;
  short hp;
  short max_hp;
  short cooldown;
  char carry_type;
  char carry_amount;
  char facing_left;
  char anim_frame;
  char anim_timer;
  char selected;
};

/* Represents an airborne ballistic arrow or fireball projectile. */
struct Projectile {
  char active;
  char team;
  int x;
  int y;
  int z;
  int vx;
  int vy;
  int vz;
  short damage;
};

/* Visual particle spark, blood drop, or dust puff. */
struct Particle {
  char active;
  int x;
  int y;
  int vx;
  int vy;
  short color;
  char life;
};

/* Temporary rising text banner displaying damage or gathered bounty. */
struct FloatText {
  char active;
  int x;
  int y;
  char text[12];
  short color;
  char life;
};

/* Persistent high score and campaign survival metrics. */
struct HighScore {
  int best_days;
  int best_score;
  int best_kills;
};

#define TOTAL_CELLS 4096
#define TOTAL_UNITS 104

/* Primary game universe state encapsulating economy, units, and environment. */
struct GameWorld {
  struct MapCell grid[TOTAL_CELLS];
  struct Building buildings[MAX_BUILDINGS];
  struct Unit units[TOTAL_UNITS];
  struct Projectile projectiles[MAX_PROJECTILES];
  struct Particle particles[MAX_PARTICLES];
  struct FloatText float_texts[MAX_FLOAT_TEXTS];
  
  /* Resources */
  int wood;
  int stone;
  int gold;
  int food;
  int population;
  int max_population;
  
  /* Progression */
  int day_number;
  int day_ticks;
  char phase;
  int score;
  int enemies_slain;
  
  /* Camera offset in pixels */
  int cam_x;
  int cam_y;
  
  /* Selection and Command state */
  int selected_count;
  char selected_building_type;
  int mouse_world_x;
  int mouse_world_y;
  int mouse_tile_x;
  int mouse_tile_y;
  
  /* Game mode flags */
  char auto_commander;
  char sound_enabled;
  char game_state;
  
  /* Persistent records */
  struct HighScore record;
};

/* World management and procedural generation routines. */
void world_init(struct GameWorld* w, unsigned seed);
struct MapCell* world_get_cell(struct GameWorld* w, int tx, int ty);
bool world_is_walkable(struct GameWorld* w, int tx, int ty);
int world_place_building(struct GameWorld* w, int type, int tx, int ty);
bool world_can_place(struct GameWorld* w, int type, int tx, int ty);
void world_update(struct GameWorld* w);

/* Entity lifecycle and simulation. */
int unit_spawn(struct GameWorld* w, int type, int team, int px, int py);
void units_update(struct GameWorld* w);
void projectiles_update(struct GameWorld* w);
void particles_update(struct GameWorld* w);
void float_texts_update(struct GameWorld* w);
void add_float_text(struct GameWorld* w, int x, int y, char* text, short color);
void add_particle(struct GameWorld* w, int x, int y, int vx, int vy, short color, int life);
void projectile_spawn(struct GameWorld* w, int team, int sx, int sy, int tx, int ty, int dmg);

/* Rendering subsystem. */
void render_init(void);
void render_frame(struct GameWorld* w, int mouse_px, int mouse_py, int drag_active, int drag_x0, int drag_y0);
void render_title_screen(struct GameWorld* w);
void render_help_screen(void);
void render_gameover_screen(struct GameWorld* w);
void render_victory_screen(struct GameWorld* w);

/* Audio synthesizer and sound effects. */
void audio_init(void);
void audio_service(struct GameWorld* w);
void audio_play_sfx(int sfx_id);
void audio_shutdown(void);

#define SFX_WOOD_CHOP   1
#define SFX_STONE_MINE  2
#define SFX_ARROW_SHOOT 3
#define SFX_SWORD_HIT   4
#define SFX_BUILD_PLACE 5
#define SFX_BELL_DUSK   6
#define SFX_FANFARE_DAWN 7
#define SFX_GOLD_CHIME  8
#define SFX_VICTORY     9
#define SFX_DEFEAT      10

/* Autonomous AI commander and governor routines. */
void ai_commander_step(struct GameWorld* w);

/* Persistence helpers. */
void save_load_record(struct HighScore* record);
void save_write_record(struct HighScore* record);

#endif /* REALM_H */
