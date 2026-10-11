#include "delve.h"

/*
 * The dungeon map and everything that lives in it.
 */

unsigned char g_map[MAP_CELLS];

/* Rows of a small hand-made test level: '#' brick, 'm' mossy brick,
 * 'r' rock, 'g' crystal rock, 'u' runes, 'k' dark brick, '.' floor. */
static char* test_rows[MAP_H] = {
  "################################",
  "#......#.......mmmmm...........#",
  "#......#.......m...m...........#",
  "#..g...#.......m...m....rrrrr..#",
  "#..............m...m....r...r..#",
  "#......#..................g.r..#",
  "#......#.......mmmmm....r...r..#",
  "####.###................rrrrr..#",
  "#..............................#",
  "#..kkkk...uuuu.................#",
  "#..k..k...u..u.................#",
  "#..k..k...u..u.......#####.....#",
  "#..kk.k...uu.u.......#...#.....#",
  "#.....................#.#......#",
  "#..............................#",
  "#..............................#",
  "################################",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "#..............................#",
  "################################",
};

/* Load the built-in test level. */
void world_init_test_map(void){
  for (int y = 0; y < MAP_H; ++y){
    char* row = test_rows[y];
    for (int x = 0; x < MAP_W; ++x){
      int cell = CELL_EMPTY;
      switch (row[x]){
        case '#': cell = TEX_BRICK + 1; break;
        case 'm': cell = TEX_BRICK_MOSS + 1; break;
        case 'r': cell = TEX_ROCK + 1; break;
        case 'g': cell = TEX_ROCK_GEM + 1; break;
        case 'u': cell = TEX_RUNES + 1; break;
        case 'k': cell = TEX_BRICK_DARK + 1; break;
        default: cell = CELL_EMPTY; break;
      }
      g_map[y * MAP_W + x] = cell;
    }
  }
}

/* Whether the player or a monster is blocked by cell (cx, cy). Cells
 * outside the map count as solid. */
bool cell_solid(int cx, int cy){
  if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return true;
  return g_map[cy * MAP_W + cx] != CELL_EMPTY;
}
