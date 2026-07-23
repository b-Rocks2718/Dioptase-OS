#include "../crt/vga.h"
#include "../crt/ps2.h"
#include "../crt/print.h"
#include "../crt/sys.h"
#include "../crt/stdbool.h"

#define GRAVITY 6
#define JUMP_VELOCITY 0x16
#define GROUND_Y 357
#define DINO_X 80

#define GROUND_SPEED 12
#define TILE_LAYER_SCALE 1
#define TILE_LAYER_SCREEN_SCALE 2
#define TILE_SIZE 8
#define GROUND_TILE_COUNT 8
#define SKY_TILE 128
#define SKY_COLOR 0x0E88
#define SKY_ROW_END 25
#define GROUND_ROW_START 25
#define GROUND_ROW_END 31
#define NUM_GAME_SPRITES 6
#define HIDDEN_SPRITE_COORD 1000

extern short DINORUNSHEET_DATA[42];
extern short SPRITEMAP_DATA[42];
extern short SUNSHEET_DATA[42];
extern short TILEMAP_DATA[42];

short* SPRITE_DATA_START = NULL; 
short* TILEMAP = NULL;
short* TILE_FB = NULL;

int dino_y;
int dino_vy;
int is_jumping;

int obstacle_1_x;
int obstacle_1_y;
int obstacle_2_x;
int obstacle_2_y;
int cloud_1_x;
int cloud_1_y;
int cloud_2_x;
int cloud_2_y;
int sun_x;
int sun_y;
unsigned frame;
unsigned frame2;
unsigned ground_scroll_screen_px;

static int read_input_event(void){
  int available = fd_bytes_available(STDIN);

  if (available > 0){
    unsigned char byte;
    if (read(STDIN, &byte, 1) == 1){
      return byte;
    }
    return 0;
  }

  if (available < 0){
    return getkey();
  }

  return 0;
}

void draw_tile(unsigned x, unsigned y, short tile){
  TILE_FB[x + TILE_ROW_WIDTH * y] = tile;
}

static void copy_tile_from_sheet(unsigned dst_tile, unsigned src_tile){
  for (int i = 0; i < TILE_SIZE; ++i){
    for (int j = 0; j < TILE_SIZE; ++j){
      short color = TILEMAP_DATA[src_tile * 64 + i * TILE_SIZE + j];
      if (color == 0x0F3F) color = 0xF000;
      TILEMAP[dst_tile * 64 + i * TILE_SIZE + j] = color;
    }
  }
}

static void fill_tile(unsigned tile, short color){
  for (int i = 0; i < TILE_SIZE; ++i){
    for (int j = 0; j < TILE_SIZE; ++j){
      TILEMAP[tile * 64 + i * TILE_SIZE + j] = color;
    }
  }
}

static void hide_game_sprites(void){
  for (unsigned sprite = 0; sprite < NUM_GAME_SPRITES; ++sprite){
    set_sprite_coords(sprite, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
  }
}

static void restore_terminal_video(void){
  set_hscroll(0);
  set_vscroll(0);
  hide_game_sprites();
  set_tile_scale(0);
  load_text_tiles();
  puts("\x1b[2J");
  puts("\x1b[H");
  puts("\x1b[?25h");
}

static void wait_for_next_vblank(void){
  unsigned frame_counter = get_vga_frame_counter();

  // Wait for the current frame to finish so the next batch of scroll/sprite
  // register writes lands during vblank instead of tearing mid-frame.
  while (get_vga_frame_counter() == frame_counter){
  }

  while (!(get_vga_status() & 1)){
  }
}

void init_dino(void){
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      int color = DINORUNSHEET_DATA[i * 32 + j];
      if (color == 0x0F3F) SPRITE_DATA_START[2 * 1024 + i * 32 + j] = 0xF000;
      else SPRITE_DATA_START[2 * 1024 + i * 32 + j] = color;
    }
  }
}


void init_ground_tiles(void){
  for (int tile = 0; tile < GROUND_TILE_COUNT; ++tile){
    copy_tile_from_sheet(tile, tile);
  }

  // Reserve one tile slot for a solid sky color instead of depending on
  // whatever the text-mode loader happened to leave in that slot.
  fill_tile(SKY_TILE, SKY_COLOR);

  for (int i = GROUND_ROW_START; i < GROUND_ROW_END; ++i){
    for (int j = 0; j < TILE_ROW_WIDTH; ++j){
      int tile = (i + j * 3) & 0x7;
      draw_tile(j, i, tile);
    }
  }
}

void init_obstacles(void){
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      short color = SPRITEMAP_DATA[i * 32 + j];
      if (color == 0x0F3F) color = 0xF000;
      SPRITE_DATA_START[i * 32 + j] = color;
    }
  }
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      short color = SPRITEMAP_DATA[1024 + i * 32 + j];
      if (color == 0x0F3F) color = 0xF000;
      SPRITE_DATA_START[1024 + i * 32 + j] = color;
    }
  }
}

int init_sun(void){
  short* p = SPRITE_DATA_START;
  // p is a short*, so pointer arithmetic is already in 16-bit pixels.
  p += 32 * 32 * 3;
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      int color = SUNSHEET_DATA[i * 32 + j];
      if (color == 0x0F3F) p[i * 32 + j] = 0xF000;
      else p[i * 32 + j] = color;
    }
  }
}

void init_sky(void){
  for (int i = 0; i < SKY_ROW_END; ++i){
    for (int j = 0; j < TILE_ROW_WIDTH; ++j){
      draw_tile(j, i, SKY_TILE);
    }
  }

  // draw clouds
  short* p = SPRITE_DATA_START;
  // p is a short*, so each sprite slot is 32*32 entries, not bytes.
  p += 32 * 32 * 4;
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      short color = SPRITEMAP_DATA[4 * 1024 + i * 32 + j];
      if (color == 0x0F3F) p[i * 32 + j] = 0xF000;
      else p[i * 32 + j] = color;
    }
  }
  p += 32 * 32;
  for (int i = 0; i < 32; ++i){
    for (int j = 0; j < 32; ++j){
      short color = SPRITEMAP_DATA[5 * 1024 + i * 32 + j];
      if (color == 0x0F3F) p[i * 32 + j] = 0xF000;
      else p[i * 32 + j] = color;
    }
  }

  init_sun();
}

void handle_physics(void){
  // physics
  dino_y -= dino_vy;
  if (is_jumping){
    dino_vy -= GRAVITY;
  } else {
    dino_vy = 0;
  }

  if (dino_y >= GROUND_Y){
    dino_y = GROUND_Y;
    dino_vy = 0;
    is_jumping = 0;
  }
}

void move_obstacles(void){
  // move obstacle
  obstacle_1_x -= GROUND_SPEED;
  if (obstacle_1_x < 0){
    obstacle_1_x = 700 + 10 * (frame % 7); // respawn offset
  }

  obstacle_2_x -= GROUND_SPEED;
  if (obstacle_2_x < 0){
    obstacle_2_x = 700 + 10 * (frame % 7); // respawn offset
  }

  cloud_1_x -= 2;
  if (cloud_1_x < 0){
    cloud_1_x = 650 + 10 * (frame % 5); // respawn offset
    cloud_1_y = 40 + 2 * (frame % 20); // respawn offset
  }

  cloud_2_x -= 1;
  if (cloud_2_x < 0){
    cloud_2_x = 680 + 10 * (frame % 5); // respawn offset
    cloud_2_y = 2 * (frame % 20); // respawn offset
  }

  // Sprite positions are tracked in screen pixels, but the tile-layer scroll
  // register is expressed in unscaled tile pixels. At scale 1 the tile layer
  // is doubled on screen, so accumulate screen-space motion and convert back
  // to the register's logical-pixel units.
  ground_scroll_screen_px += GROUND_SPEED;
  set_hscroll(-(ground_scroll_screen_px / TILE_LAYER_SCREEN_SCALE));
}

int score;

int handle_collisions(void){
  // collision
  if (obstacle_1_x <= DINO_X + 1 && obstacle_1_x + 12 >= DINO_X){
    if (obstacle_1_y - 20 < dino_y){
      return 1; 
    } else {
      score += 1;
    }
  }
  if (obstacle_2_x <= DINO_X + 1 && obstacle_2_x + 20 >= DINO_X){
    if (obstacle_2_y - 10 < dino_y){
      return 1;
    } else {
      score += 1;
    }
  }
  return 0;
}

void update_positions(){
  set_sprite_coords(0, obstacle_1_x / 2, obstacle_1_y / 2);
  set_sprite_coords(1, obstacle_2_x / 2, obstacle_2_y / 2);
  set_sprite_coords(2, DINO_X / 2, dino_y / 2);
  set_sprite_coords(4, cloud_1_x / 2, cloud_1_y / 2);
  set_sprite_coords(5, cloud_2_x / 2, cloud_2_y / 2);
}

extern void do_animations(void);

unsigned main(void){
  // Hide the terminal cursor and clear the terminal before we take over the VGA
  // tile layer directly. Doing this after custom tile writes can race with the
  // terminal's own framebuffer updates and stomp the game background.
  puts("\x1b[?25l\x1b[2J");

  SPRITE_DATA_START = get_spritemap(); 
  TILEMAP = get_tilemap();
  TILE_FB = get_tile_fb();

  load_text_tiles_colored(0x000, SKY_COLOR);
  init_dino();
  init_obstacles();
  init_ground_tiles();

  start: dino_y = 300;
  dino_vy = 0;
  is_jumping = 1;
  score = 0;

  obstacle_1_x = 300;
  obstacle_1_y = 352;
  obstacle_2_x = 600;
  obstacle_2_y = 352;
  cloud_1_x = 100;
  cloud_1_y = 60;
  cloud_2_x = 150;
  cloud_2_y = 40;
  sun_x = 450;
  sun_y = 15;
  frame = 0;
  ground_scroll_screen_px = 0;
  set_hscroll(0);
  set_vscroll(0);
  
  init_sky();

  set_tile_scale(TILE_LAYER_SCALE);
  set_sprite_scale(0, 1);
  set_sprite_scale(1, 1);
  set_sprite_scale(2, 1);
  set_sprite_scale(3, 1);
  set_sprite_scale(4, 1);
  set_sprite_scale(5, 1);

  set_sprite_coords(3, sun_x / 2, sun_y / 2);

  while (true){
    wait_for_next_vblank();
    update_positions();

    // input
    unsigned key = read_input_event();
    if (key == 0x71) {
      restore_terminal_video();
      return 0;
    }
    if (key == 0x20 && !is_jumping){ // spacebar
      dino_vy = JUMP_VELOCITY;
      dino_y -= 3;
      is_jumping = 1;
    }

    handle_physics();

    move_obstacles();

    if (handle_collisions()){
      // game over
      set_hscroll(0);
      puts("Game Over!\n");
      puts("Score: ");
      print_unsigned(score);
      // home cursor
      puts("\x1b[H");

      // wait a bit, then drain key buffer
      sleep(10);
      while (read_input_event() != 0);
      while (1){
        // input
        unsigned key = read_input_event();
        if (key == 'q') {
          restore_terminal_video();
          return 0;
        }
        if (key != 0) goto start;
        frame++;
        sleep(5);
      }
    }

    if (!(frame % 3)){
      do_animations();
    }

    frame++;
  }
}
