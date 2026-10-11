#ifndef LANTERN_H
#define LANTERN_H

/*
 * Shared state for the lantern vault. The pixel view is the top-left 80x60
 * of the 320-wide framebuffer at pixel scale 2, which hardware shows at 8x
 * (docs/mem_map.md). Each column covers two angle units, so the field of view
 * matches a 160-wide view without writing four times as many pixels.
 * World positions are 8.8 fixed point: 256 is one map cell.
 * Angles wrap at 1024, with 0 facing +x and positive angles turning toward +y.
 */

#define VIEW_W 80
#define VIEW_H 60
#define VIEW_CENTER 40
#define COL_STEP 2
#define PIXEL_STRIDE 320
#define PIXEL_SCALE_PLAY 2

#define MAP_W 32
#define MAP_H 24
#define FP_ONE 256
#define ANGLE_FULL 1024
#define ANGLE_RIGHT 256

#define MAX_ACTORS 16

#define CELL_STONE 0
#define CELL_BRICK 1
#define CELL_METAL 2
#define CELL_FLOOR 3
#define CELL_DOOR 4
#define CELL_GATE 5
#define CELL_EXIT 6

#define KIND_SENTRY 0
#define KIND_CORE 1
#define KIND_HEALTH 2
#define KIND_CRYSTAL 3

#define STATE_TITLE 0
#define STATE_PLAY 1
#define STATE_PAUSE 2
#define STATE_DEAD 3
#define STATE_WON 4

#define SFX_SHOT 1
#define SFX_HIT 2
#define SFX_HURT 3
#define SFX_DOOR 4
#define SFX_CORE 5
#define SFX_PICKUP 6
#define SFX_STEP 7
#define SFX_WIN 8
#define SFX_DIE 9

#define SPRITE_MUZZLE 13
#define SPRITE_CROSS 14
#define SPRITE_LANTERN 15

/* Bit 8 of a PS/2 event word marks a release (docs/mem_map.md). */
#define KEY_RELEASE_BIT 0x0100

/* One enemy, core, or pickup. x and y are cell centers in 8.8 fixed point. */
struct Actor {
  int x;
  int y;
  int kind;
  int hp;
  int alive;
  int hurt;
  int aggro;
  int cooldown;
};

extern int cells[MAP_H][MAP_W];
extern struct Actor actors[MAX_ACTORS];
extern int actor_count;

extern int player_x;
extern int player_y;
extern int player_angle;
extern int player_hp;
extern int player_moving;
extern int cores_left;
extern int cores_total;
extern int exit_x;
extern int exit_y;
extern int hurt_timer;
extern int world_dirty;
extern int shot_flash;
extern unsigned lantern_time;
extern char message[80];
extern unsigned message_until;
extern int lantern_bench;

/* Unit facing and right vectors in 8.8, refreshed from player_angle. */
extern int lantern_dir_x;
extern int lantern_dir_y;
extern int lantern_right_x;
extern int lantern_right_y;

/* Build the vault, actors, and player from the baked map. Returns 0 on success. */
int load_level(void);

/* Advance combat, doors, and movement. Returns PLAY, DEAD, or WON. */
int update_world(int dt, int forward, int back, int left, int right, int turn, int use, int fire);

/* Recompute the 8.8 facing vectors from player_angle. */
void render_sync_facing(void);

/* Remember a line of HUD text for about `frames` display frames. */
void set_message(char* text, unsigned frames);

/* Allocate the view buffers, fill the trig and texture tables, and upload sprites. */
int render_init(volatile short* pixels, volatile short* tiles, volatile short* sprites);

/* Raycast, draw billboards, and flush changed pixels to the framebuffer. */
void render_frame(int state, int show_map);

/* Move the hardware sprites off the screen before the shell reclaims the console. */
void render_hide_sprites(void);

/* Sum the composed view so a headless bench can check that the picture changes. */
unsigned view_checksum(void);

/* Print a color-letter picture of one rectangle of the composed view. */
void view_preview_box(int x0, int y0, int w, int h);

/* Print the whole composed view as color letters. */
void view_preview(void);

/* Claim the audio device and start the drone. Failure leaves the game silent. */
void audio_start(void);

/* Queue more music and sound effects if the playback ring has room. */
void audio_pump(void);

/* Replace the current sound effect. Footsteps yield to anything already playing. */
void sfx_play(int kind);

/* Stop playback and release the device if this process owns it. */
void audio_stop(void);

#endif
