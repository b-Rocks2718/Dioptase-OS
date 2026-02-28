#include "../kernel/vga.h"
#include "../kernel/ps2.h"
#include "../kernel/print.h"

#define GRAVITY 2
#define JUMP_VELOCITY 0x030
#define GROUND_Y 357
#define DINO_X 80

short* SCROLL_X = (short*)0x7FE5B40;
short* SPRITE_DATA_START = (short*)0x7FF0000; 
short* SPRITE_0_X = (short*)0x7FE5B00;
short* SPRITE_0_Y = (short*)0x7FE5B02;
short* SPRITE_1_X = (short*)0x7FE5B04;
short* SPRITE_1_Y = (short*)0x7FE5B06;
short* SPRITE_2_X = (short*)0x7FE5B08;
short* SPRITE_2_Y = (short*)0x7FE5B0A;
short* SPRITE_3_X = (short*)0x7FE5B0C;
short* SPRITE_3_Y = (short*)0x7FE5B0E;
short* SPRITE_4_X = (short*)0x7FE5B10;
short* SPRITE_4_Y = (short*)0x7FE5B12;
short* SPRITE_5_X = (short*)0x7FE5B14;
short* SPRITE_5_Y = (short*)0x7FE5B16;

char* SPRITE_0_SCALE = (char*)0x7FE5B60;
char* SPRITE_1_SCALE = (char*)0x7FE5B61;
char* SPRITE_2_SCALE = (char*)0x7FE5B62;
char* SPRITE_3_SCALE = (char*)0x7FE5B63;
char* SPRITE_4_SCALE = (char*)0x7FE5B64;
char* SPRITE_5_SCALE = (char*)0x7FE5B65;

extern short SUNSHEET_DATA[42];
extern short DINORUNSHEET_DATA[42];

extern short TILEMAP_DATA[42];
extern short SPRITEMAP_DATA[42];

// variables
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

void draw_pixel(unsigned x, unsigned y, short new){
  TILE_FB[x + TILE_ROW_WIDTH * y] = new;
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
  // load tilemap
  for (int tile = 0; tile < 1; ++tile){
    for (int i = 0; i < 8; ++i){
      for (int j = 0; j < 8; ++j){
        short color = TILEMAP_DATA[tile * 64 + i * 8 + j];
        if (color == 0x0F3F) color = 0xF000;
        TILEMAP[8 * i + j + tile * 64] = color;
      }
    }
  }

  for (int i = 24; i < 27; ++i){
    for (int j = 0; j < TILE_ROW_WIDTH; ++j){
      int tile = (i + j * 3) & 0x7;
      draw_pixel(j, i, tile);
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
  for (int i = 0; i <= 23; ++i){
    for (int j = 0; j < TILE_ROW_WIDTH; ++j){
      draw_pixel(j, i, 128);
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
  obstacle_1_x -= 6;
  if (obstacle_1_x < 0){
    obstacle_1_x = 700 + 10 * (frame % 7); // respawn offset
  }

  obstacle_2_x -= 6;
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

  *SCROLL_X = *SCROLL_X - 6;
}

int score;

int handle_collisions(void){
  // collision
  if (obstacle_1_x <= DINO_X + 1 && obstacle_1_x + 6 >= DINO_X){
    if (obstacle_1_y - 10 < dino_y){
      return 1; 
    } else {
      score += 1;
    }
  }
  if (obstacle_2_x <= DINO_X + 1 && obstacle_2_x + 10 >= DINO_X){
    if (obstacle_2_y - 5 < dino_y){
      return 1;
    } else {
      score += 1;
    }
  }
  return 0;
}

void update_positions(){
  *SPRITE_0_X = obstacle_1_x;
  *SPRITE_0_Y = obstacle_1_y;

  *SPRITE_1_X = obstacle_2_x;
  *SPRITE_1_Y = obstacle_2_y;
  
  *SPRITE_2_X = DINO_X;
  *SPRITE_2_Y = dino_y;

  *SPRITE_4_X = cloud_1_x;
  *SPRITE_4_Y = cloud_1_y;

  *SPRITE_5_X = cloud_2_x;
  *SPRITE_5_Y = cloud_2_y;
}

void do_animations(void);

unsigned kernel_main(void){
  load_text_tiles_colored(0x000, 0xe88);
  init_dino();
  init_obstacles();
  init_ground_tiles();

  start: dino_y = 300;
  dino_vy = 0;
  is_jumping = 1;

  obstacle_1_x = 300;
  obstacle_1_y = 352;
  obstacle_2_x = 600;
  obstacle_2_y = 352;
  cloud_1_x = 100;
  cloud_1_y = 60;
  cloud_2_x = 150;
  cloud_2_y = 40;
  sun_x = 560;
  sun_y = 30;
  frame = 0;

  clear_screen();
  init_sky();

  *TILE_SCALE = 1;
  *SPRITE_0_SCALE = 0;
  *SPRITE_1_SCALE = 0;
  *SPRITE_2_SCALE = 0;
  *SPRITE_3_SCALE = 0;
  *SPRITE_4_SCALE = 0;
  *SPRITE_5_SCALE = 0;

  *SPRITE_3_X = sun_x;
  *SPRITE_3_Y = sun_y;

  while (1){
    update_positions();

    // input
    unsigned key = getkey();
    if (key == 0x71) return 0; // 'q' to quit
    if (key == 0x20 && !is_jumping){ // spacebar
      dino_vy = 13;//JUMP_VELOCITY;
      dino_y -= 3;
      is_jumping = 1;
    }

    handle_physics();

    move_obstacles();

    if (handle_collisions()){
      // game over
      *SCROLL_X = 0;
      puts("Game Over!\n");
      print_unsigned(score);
      while (1){
        // input
        unsigned key = getkey();
        if (key == 0x71) return 0; // 'q' to quit
        if (key != 0) goto start;
        frame++;
      }
    }

    if (!(frame % 10)){
      do_animations();
    }

    frame++;
    for (unsigned delay = 0; delay < 40000; ++delay)
      ;//for (unsigned delay = 0; delay < 3; ++delay);
  }
}
