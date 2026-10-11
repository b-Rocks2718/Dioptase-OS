#ifndef LUMEN_GAME_H
#define LUMEN_GAME_H

#define WORLD_W 21
#define WORLD_H 19
#define WORLD_CELLS 399 /* WORLD_W * WORLD_H; bcc requires a literal struct extent. */
#define MAX_ENTITIES 40
#define SECTOR_COUNT 3

/* A full clockwise heading has 256 units; eight units is one gentle turn. */
#define TURN_STEP 8

#define CELL_FLOOR 0
#define CELL_WALL 1
#define CELL_GLASS 2
#define CELL_DOOR 3
#define CELL_EXIT 4

#define ENTITY_RELAY 1
#define ENTITY_DRONE 2
#define ENTITY_MEDKIT 3
#define ENTITY_CELL 4
#define ENTITY_LOG 5

#define MODE_TITLE 0
#define MODE_PLAY 1
#define MODE_UPGRADE 2
#define MODE_DEAD 3
#define MODE_WON 4

#define ACT_FORWARD 1
#define ACT_BACK 2
#define ACT_LEFT 3
#define ACT_RIGHT 4
#define ACT_STRAFE_LEFT 5
#define ACT_STRAFE_RIGHT 6
#define ACT_FIRE 7
#define ACT_USE 8
#define ACT_WAIT 9

#define SOUND_NONE 0
#define SOUND_STEP 1
#define SOUND_SHOT 2
#define SOUND_HIT 3
#define SOUND_RELAY 4
#define SOUND_PICKUP 5
#define SOUND_HURT 6
#define SOUND_EXIT 7

/* Objects occupy floor cells; disabled relays remain as physical pylons.
 * Relay and sentry beam hitboxes fill their cells, giving forgiving aim even
 * though their rendered silhouettes are narrower than a passage. */
struct Entity {
    int type;
    int x;
    int y;
    int color;
    int health;
    int active;
    int alert;
};

/* Complete deterministic expedition state; one foreground user thread owns it. */
struct Game {
    unsigned seed;
    unsigned rng;
    unsigned turns;
    int sector; /* Deck numbering is one-based: 1 through SECTOR_COUNT. */
    int mode;
    int x;
    int y;
    int angle; /* Clockwise heading: north 0, east 64, south 128, west 192. */
    int hp;
    int max_hp;
    int energy;
    int max_energy;
    int power;
    int regen;
    int color;
    int relays;
    int kills;
    int secrets;
    int score;
    int sound;
    int flash;
    int shot;
    int entity_count;
    int exit_x;
    int exit_y;
    unsigned char cells[WORLD_CELLS];
    unsigned char seen[WORLD_CELLS];
    struct Entity entities[MAX_ENTITIES];
    char message[96];
    char detail[96];
};

/* Return a Q10 sine for a wrapping 256-unit angle, shared by beams and camera. */
int game_sine(int angle);
/* Start a replayable three-sector expedition; seed zero is also accepted. */
void game_start(struct Game* game, unsigned seed);
/* Apply a discrete action, advancing enemies only for actions that cost a turn. */
int game_act(struct Game* game, int action);
/* Apply one of three between-sector upgrades and enter the next sector. */
void game_upgrade(struct Game* game, int choice);
/* Execute one demonstration decision through the ordinary player actions. */
void game_autoplay(struct Game* game);
/* Query bounded map coordinates; outside the map is always solid. */
int game_cell(struct Game* game, int x, int y);
/* Return an active object at a cell, or -1 when no object occupies it. */
int game_entity(struct Game* game, int x, int y);
/* Verify reachability, boundaries, and legal object placement for diagnostics. */
int game_validate(struct Game* game);
/* Run deterministic gameplay checks; return zero on success. */
int game_selftest(void);

#endif
