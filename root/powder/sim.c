/* Powderworks simulation: integer cellular rules for every element pair. */
#include "sim.h"

/* Cell type grid, row-major, owned by the simulation. */
unsigned char sim_cells[SIM_CELLS];
/* Per-cell auxiliary byte: lifetime for fire/smoke, heat for lava/ember, growth timer for plant, shade seed otherwise. */
unsigned char sim_aux[SIM_CELLS];
/* Per-step marker so a moved cell is never moved twice in one step. */
unsigned char sim_moved[SIM_CELLS];
/* Live cell count per element code, refreshed by sim_recount(). */
int sim_counts[EL_COUNT];
/* Monotonic simulation step counter, used for flicker and slow elements. */
unsigned sim_steps;

/* Shared xorshift state; the zero seed is remapped so the generator never sticks. */
static unsigned rng_state = 1;

/* Generation stamped into sim_moved each step, so no clearing pass is needed. */
static unsigned char move_gen = 1;

/* Display names indexed by element code; BRUSH_ERASE is handled by name lookup. */
static char* element_names[EL_COUNT] = {
  "VOID", "SAND", "WATER", "OIL", "LAVA", "FIRE", "EMBER",
  "WOOD", "STONE", "ACID", "SNOW", "VINE", "SMOKE",
};

/* Packed 0x0BGR shades (dark to bright) indexed by element code. */
static short element_colors[EL_COUNT][4] = {
  {0x000, 0x000, 0x000, 0x000},
  {0x39A, 0x3AD, 0x3BE, 0x3CF},
  {0xC41, 0xD52, 0xD62, 0xE73},
  {0x135, 0x146, 0x157, 0x168},
  {0x05E, 0x06F, 0x18F, 0x2CF},
  {0x008, 0x029, 0x06E, 0x0FF},
  {0x036, 0x048, 0x05A, 0x06C},
  {0x136, 0x147, 0x158, 0x169},
  {0x444, 0x666, 0x888, 0x999},
  {0x2C3, 0x2D3, 0x2E4, 0x3F5},
  {0xDCC, 0xEDD, 0xFEE, 0xFFF},
  {0x282, 0x3A3, 0x3C3, 0x4D4},
  {0x444, 0x555, 0x666, 0x777},
};

/* RGB332 tile color byte per element, used for palette swatches. */
static int element_tile_colors[EL_COUNT] = {
  0x00, 0xF4, 0x17, 0x92, 0xE0, 0xF0, 0xE8,
  0x92, 0x92, 0x3C, 0xFF, 0x3C, 0x6D,
};

/* Advance the shared xorshift generator and return its next value. */
unsigned sim_rand(void) {
  unsigned x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

/* Flat grid index of a cell; 160 is (1<<7)+(1<<5), so no multiply is needed. */
static int cell_index(int x, int y) {
  return (y << 7) + (y << 5) + x;
}

/* Return the element at a cell, or EL_STONE outside the grid. */
int sim_at(int x, int y) {
  if (x < 0 || x >= SIM_W || y < 0 || y >= SIM_H) return EL_STONE;
  return sim_cells[cell_index(x, y)];
}

/* Set a cell and its auxiliary byte without touching the moved marker. */
static void set_cell(int x, int y, int element, int aux) {
  int i = cell_index(x, y);
  sim_cells[i] = (unsigned char)element;
  sim_aux[i] = (unsigned char)aux;
}

/* Swap two cells by flat index and stamp both with this step's generation. */
static void swap_cells(int a, int b) {
  unsigned char t = sim_cells[a];
  sim_cells[a] = sim_cells[b];
  sim_cells[b] = t;
  t = sim_aux[a];
  sim_aux[a] = sim_aux[b];
  sim_aux[b] = t;
  sim_moved[a] = move_gen;
  sim_moved[b] = move_gen;
}

/* Short display name of an element or brush code (palette order matches). */
char* sim_element_name(int element) {
  if (element == BRUSH_ERASE) return "ERASE";
  if (element < 0 || element >= EL_COUNT) return "VOID";
  return element_names[element];
}

/* Packed 0x0BGR framebuffer color of an element at a shade 0..3. */
short sim_element_color(int element, int shade) {
  if (element < 0 || element >= EL_COUNT) return 0;
  if (shade < 0) shade = 0;
  if (shade > 3) shade = 3;
  return element_colors[element][shade];
}

/* RGB332 tile color byte used for palette swatches of an element. */
int sim_element_tile_color(int element) {
  if (element == BRUSH_ERASE) return 0xFF;
  if (element < 0 || element >= EL_COUNT) return 0;
  return element_tile_colors[element];
}

/* Precomputed background colors; the empty-cell pattern never changes. */
static short bg_colors[SIM_CELLS];
/* Whether bg_colors has been built yet (built lazily on first render). */
static int bg_ready = 0;

/* Build the static background pattern once, with the plain formula. */
static void build_background(void) {
  for (int y = 0; y < SIM_H; y++) {
    int base = (y << 7) + (y << 5);
    for (int x = 0; x < SIM_W; x++) {
      short color = (short)(((x * 31 + y * 17) & 127) == 0
                                ? 0x334
                                : (((x ^ y) & 1) != 0 ? 0x300 : 0x200));
      bg_colors[base + x] = color;
    }
  }
  bg_ready = 1;
}

/* Fill one row of framebuffer colors, with background and animation shades. */
void sim_row_colors(int y, short* out) {
  if (!bg_ready) build_background();
  int base = (y << 7) + (y << 5);
  for (int x = 0; x < SIM_W; x++) {
    int i = base + x;
    int t = sim_cells[i];
    short color;
    if (t == EL_EMPTY) {
      color = bg_colors[i];
    } else if (t == EL_FIRE) {
      color = element_colors[t][((sim_steps >> 1) + (unsigned)(x + y)) & 3];
    } else if (t == EL_LAVA) {
      color = element_colors[t][(((x >> 2) + (int)(sim_steps >> 3)) & 1) != 0 ? 3 : 2];
    } else if (t == EL_SMOKE) {
      int aux = sim_aux[i];
      color = element_colors[t][aux > 28 ? 3 : (aux > 14 ? 2 : 1)];
    } else if (t == EL_WATER || t == EL_OIL || t == EL_ACID) {
      int shade = 1 + (int)(((unsigned)(x + y) + sim_steps) & 1);
      if ((sim_aux[i] & 3) == 3) shade = 3;
      color = element_colors[t][shade];
    } else {
      color = element_colors[t][sim_aux[i] & 3];
    }
    out[x] = color;
  }
}

/* Build the stone tray: side walls and a two-row floor the powder piles on. */
static void build_tray(void) {
  for (int y = PLAY_Y0; y < SIM_H; y++) {
    set_cell(0, y, EL_STONE, 1);
    set_cell(SIM_W - 1, y, EL_STONE, 1);
  }
  for (int x = 0; x < SIM_W; x++) {
    set_cell(x, SIM_H - 2, EL_STONE, 2);
    set_cell(x, SIM_H - 1, EL_STONE, 1);
  }
}

/* Remove every element and rebuild the stone tray. */
void sim_clear(void) {
  for (int i = 0; i < SIM_CELLS; i++) {
    sim_cells[i] = EL_EMPTY;
    sim_aux[i] = 0;
    sim_moved[i] = 0;
  }
  build_tray();
  sim_recount();
}

/* Reset the world to an empty tray and seed the shared random generator. */
void sim_init(unsigned seed) {
  rng_state = seed != 0 ? seed : 0x9E3779B9u;
  sim_steps = 0;
  sim_clear();
}

/* Recompute sim_counts from scratch; also repairs tray cells eaten away. */
void sim_recount(void) {
  for (int e = 0; e < EL_COUNT; e++) sim_counts[e] = 0;
  for (int i = 0; i < SIM_CELLS; i++) sim_counts[sim_cells[i]]++;
  /* The outer tray is structural: refill any frame cell that was erased. */
  for (int y = PLAY_Y0; y < SIM_H; y++) {
    int left = cell_index(0, y);
    int right = cell_index(SIM_W - 1, y);
    if (sim_cells[left] == EL_EMPTY) set_cell(0, y, EL_STONE, 1);
    if (sim_cells[right] == EL_EMPTY) set_cell(SIM_W - 1, y, EL_STONE, 1);
  }
  for (int x = 0; x < SIM_W; x++) {
    if (sim_cells[cell_index(x, SIM_H - 2)] == EL_EMPTY) {
      set_cell(x, SIM_H - 2, EL_STONE, 2);
    }
    if (sim_cells[cell_index(x, SIM_H - 1)] == EL_EMPTY) {
      set_cell(x, SIM_H - 1, EL_STONE, 1);
    }
  }
}

/* Paint a filled disc of an element (or BRUSH_ERASE) clipped to the tray. */
void sim_paint(int cx, int cy, int radius, int element) {
  if (radius < 0) radius = 0;
  int r2 = radius * radius;
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      if (dx * dx + dy * dy > r2) continue;
      int x = cx + dx;
      int y = cy + dy;
      if (x < PLAY_X0 || x > PLAY_X1 || y < PLAY_Y0 || y > SIM_H - 3) continue;
      int painted = element == BRUSH_ERASE ? EL_EMPTY : element;
      int idx = cell_index(x, y);
      if (sim_cells[idx] != painted) {
        sim_counts[sim_cells[idx]]--;
        sim_counts[painted]++;
      }
      int aux = (int)(sim_rand() & 3);
      if (painted == EL_FIRE) aux = 25 + (int)(sim_rand() & 15);
      if (painted == EL_SMOKE) aux = 40 + (int)(sim_rand() & 31);
      if (painted == EL_EMBER) aux = 220;
      if (painted == EL_LAVA) aux = 255;
      set_cell(x, y, painted, aux);
    }
  }
}

/* True when a falling cell may enter the target: empty, or smoke/fire it displaces. */
static int fall_target_ok(int element) {
  return element == EL_EMPTY || element == EL_SMOKE || element == EL_FIRE;
}

/* Fall from flat index i at column x: straight down, else a random diagonal. */
static int fall_down_at(int i, int x) {
  int below = i + SIM_W;
  if (fall_target_ok(sim_cells[below])) {
    swap_cells(i, below);
    return 1;
  }
  int dir = (sim_rand() & 1) != 0 ? 1 : -1;
  for (int k = 0; k < 2; k++) {
    int nx = x + dir;
    if (nx >= 0 && nx < SIM_W && fall_target_ok(sim_cells[below + dir]) &&
        fall_target_ok(sim_cells[i + dir])) {
      swap_cells(i, below + dir);
      return 1;
    }
    dir = -dir;
  }
  return 0;
}

/* Spread the liquid at flat index i sideways toward the first open cell. */
static int flow_sideways_at(int i, int x, int reach) {
  int dir = (sim_rand() & 1) != 0 ? 1 : -1;
  for (int k = 0; k < 2; k++) {
    int ni = i;
    int nx = x;
    for (int d = 0; d < reach; d++) {
      ni += dir;
      nx += dir;
      if (nx < 0 || nx >= SIM_W) break;
      if (!fall_target_ok(sim_cells[ni])) break;
      if (d == reach - 1 || fall_target_ok(sim_cells[ni + SIM_W])) {
        swap_cells(i, ni);
        return 1;
      }
    }
    dir = -dir;
  }
  return 0;
}

/* Ignite the flammable cell at flat index n with probability 1 in (mask+1). */
static void try_ignite_at(int n, int mask) {
  int t = sim_cells[n];
  if (t != EL_WOOD && t != EL_OIL && t != EL_PLANT) return;
  if ((sim_rand() & (unsigned)mask) == 0) {
    sim_cells[n] = EL_FIRE;
    sim_aux[n] = (unsigned char)(25 + (int)(sim_rand() & 15));
  }
}

/* Advance falling and flowing cells; scans bottom-up so piles settle in one pass. */
static void step_fallers(void) {
  for (int y = SIM_H - 2; y >= 0; y--) {
    int base = (y << 7) + (y << 5);
    int left_first = ((sim_steps + (unsigned)y) & 1) == 0;
    for (int s = 0; s < SIM_W; s++) {
      int x = left_first ? s : SIM_W - 1 - s;
      int i = base + x;
      int t = sim_cells[i];
      if (t == EL_EMPTY || sim_moved[i] == move_gen) continue;
      if (t == EL_STONE || t == EL_WOOD) continue; /* Statics never fall. */
      if (t == EL_SAND || t == EL_EMBER) {
        int below = sim_cells[i + SIM_W];
        if (t == EL_SAND &&
            (below == EL_WATER || below == EL_OIL || below == EL_ACID)) {
          swap_cells(i, i + SIM_W); /* Sand sinks through lighter liquids. */
        } else {
          fall_down_at(i, x);
        }
      } else if (t == EL_WATER || t == EL_ACID) {
        if (t == EL_WATER && sim_cells[i + SIM_W] == EL_OIL) {
          swap_cells(i, i + SIM_W); /* Water sinks under oil, so oil floats. */
        } else if (!fall_down_at(i, x)) {
          flow_sideways_at(i, x, 3);
        }
      } else if (t == EL_OIL) {
        if (!fall_down_at(i, x)) flow_sideways_at(i, x, 2);
      } else if (t == EL_LAVA) {
        /* Lava is thick: it only moves on alternating steps. */
        if (((x + (int)sim_steps) & 1) == 0 && !fall_down_at(i, x)) {
          flow_sideways_at(i, x, 1);
        }
      } else if (t == EL_SNOW) {
        /* Snow falls at half rate, which makes soft, steep piles. */
        if (((x + y + (int)sim_steps) & 1) == 0) fall_down_at(i, x);
      }
    }
  }
}

/* Advance rising cells and every reaction that needs neighbouring heat. */
static void step_risers_and_reactions(void) {
  for (int y = 0; y < SIM_H - 1; y++) {
    int base = (y << 7) + (y << 5);
    for (int x = 0; x < SIM_W; x++) {
      int i = base + x;
      int t = sim_cells[i];
      /* Only hot, acidic, growing, and melting cells react in this pass. */
      if (t == EL_EMPTY || t == EL_SAND || t == EL_WATER || t == EL_OIL ||
          t == EL_WOOD || t == EL_STONE) {
        continue;
      }
      if (t == EL_FIRE) {
        if (sim_aux[i] > 0) sim_aux[i]--;
        if (x + 1 < SIM_W) try_ignite_at(i + 1, 15);
        if (x > 0) try_ignite_at(i - 1, 15);
        try_ignite_at(i + SIM_W, 15);
        if (y > 0) try_ignite_at(i - SIM_W, 15);
        if (sim_aux[i] == 0) {
          if ((sim_rand() & 3) == 0) {
            sim_cells[i] = EL_SMOKE;
            sim_aux[i] = 30;
          } else {
            sim_cells[i] = EL_EMPTY;
            sim_aux[i] = 0;
          }
        } else if (sim_moved[i] != move_gen && y > 0) {
          /* Flames climb into empty space and shed smoke as they go. */
          int above = sim_cells[i - SIM_W];
          if (above == EL_EMPTY && (sim_rand() & 3) == 0) {
            sim_cells[i - SIM_W] = EL_SMOKE;
            sim_aux[i - SIM_W] = (unsigned char)(25 + (int)(sim_rand() & 15));
            above = EL_SMOKE;
          }
          if (above == EL_SMOKE || above == EL_FIRE) {
            swap_cells(i, i - SIM_W);
          } else if (above == EL_EMPTY && (sim_rand() & 1) == 0) {
            swap_cells(i, i - SIM_W);
          }
        }
      } else if (t == EL_SMOKE) {
        if (sim_aux[i] > 0) sim_aux[i]--;
        if (sim_aux[i] == 0) {
          sim_cells[i] = EL_EMPTY;
          sim_aux[i] = 0;
        } else if (sim_moved[i] != move_gen && y > 0) {
          if (sim_cells[i - SIM_W] == EL_EMPTY) {
            swap_cells(i, i - SIM_W);
          } else {
            flow_sideways_at(i, x, 1);
          }
        }
      } else if (t == EL_LAVA) {
        /* Lava quenches to stone in water and lights what it touches. */
        int wet = sim_cells[i + SIM_W] == EL_WATER ||
                  (x + 1 < SIM_W && sim_cells[i + 1] == EL_WATER) ||
                  (x > 0 && sim_cells[i - 1] == EL_WATER) ||
                  (y > 0 && sim_cells[i - SIM_W] == EL_WATER);
        if (wet) {
          sim_cells[i] = EL_STONE;
          sim_aux[i] = 1;
          if (y > 0 && sim_cells[i - SIM_W] == EL_EMPTY) {
            sim_cells[i - SIM_W] = EL_SMOKE; /* The hiss becomes steam. */
            sim_aux[i - SIM_W] = 40;
          }
        } else {
          if (x + 1 < SIM_W) try_ignite_at(i + 1, 31);
          if (x > 0) try_ignite_at(i - 1, 31);
          try_ignite_at(i + SIM_W, 31);
          if (y > 0) try_ignite_at(i - SIM_W, 31);
        }
      } else if (t == EL_EMBER) {
        if (x + 1 < SIM_W) try_ignite_at(i + 1, 31);
        if (x > 0) try_ignite_at(i - 1, 31);
        try_ignite_at(i + SIM_W, 31);
        if (sim_aux[i] > 0) sim_aux[i]--;
        if (sim_aux[i] == 0) {
          sim_cells[i] = EL_STONE; /* A spent ember is ash. */
          sim_aux[i] = 0;
        }
      } else if (t == EL_ACID) {
        /* Acid eats one neighbouring solid, then spends itself as smoke. */
        int eaten = -1;
        int below = sim_cells[i + SIM_W];
        if (below == EL_SAND || below == EL_WOOD || below == EL_PLANT) {
          eaten = i + SIM_W;
        } else if (x + 1 < SIM_W && sim_cells[i + 1] == EL_STONE) {
          eaten = i + 1;
        } else if (x > 0 && sim_cells[i - 1] == EL_STONE) {
          eaten = i - 1;
        }
        if (eaten >= 0 && (sim_rand() & 7) == 0) {
          sim_cells[eaten] = EL_EMPTY;
          sim_aux[eaten] = 0;
          sim_cells[i] = EL_SMOKE;
          sim_aux[i] = 20;
        }
      } else if (t == EL_PLANT) {
        /* Vines climb into empty cells while any surrounding cell holds water. */
        int fed = sim_cells[i + SIM_W] == EL_WATER ||
                  (x + 1 < SIM_W && sim_cells[i + 1] == EL_WATER) ||
                  (x > 0 && sim_cells[i - 1] == EL_WATER) ||
                  (y > 0 && sim_cells[i - SIM_W] == EL_WATER) ||
                  (x + 1 < SIM_W && sim_cells[i + SIM_W + 1] == EL_WATER) ||
                  (x > 0 && sim_cells[i + SIM_W - 1] == EL_WATER) ||
                  (y > 0 && x + 1 < SIM_W && sim_cells[i - SIM_W + 1] == EL_WATER) ||
                  (y > 0 && x > 0 && sim_cells[i - SIM_W - 1] == EL_WATER);
        if (fed && (sim_rand() & 63) == 0) {
          if (y > 0 && sim_cells[i - SIM_W] == EL_EMPTY) {
            sim_cells[i - SIM_W] = EL_PLANT;
            sim_aux[i - SIM_W] = 0;
          } else {
            int dir = (sim_rand() & 1) != 0 ? 1 : -1;
            int nx = x + dir;
            if (nx >= 0 && nx < SIM_W && sim_cells[i + dir] == EL_EMPTY) {
              sim_cells[i + dir] = EL_PLANT;
              sim_aux[i + dir] = 0;
            }
          }
        }
      } else if (t == EL_SNOW) {
        /* Snow touching heat melts where it lies. */
        if ((x + 1 < SIM_W && sim_cells[i + 1] == EL_FIRE) ||
            (x > 0 && sim_cells[i - 1] == EL_LAVA) ||
            sim_cells[i + SIM_W] == EL_LAVA ||
            (y > 0 && sim_cells[i - SIM_W] == EL_FIRE) ||
            sim_cells[i + SIM_W] == EL_EMBER) {
          sim_cells[i] = EL_WATER;
          sim_aux[i] = 1;
        }
      }
    }
  }
}

/* Advance the whole world by one cellular step. */
void sim_step(void) {
  move_gen++;
  if (move_gen == 0) {
    for (int i = 0; i < SIM_CELLS; i++) sim_moved[i] = 0;
    move_gen = 1;
  }
  step_fallers();
  step_risers_and_reactions();
  sim_steps++;
}

/* Build the showcase scene used by the title screen and the D key. */
void sim_demo_scene(void) {
  sim_clear();
  /* A wooden house on the tray floor, with snow on its roof. */
  for (int x = 62; x <= 98; x++) set_cell(x, 88, EL_WOOD, 2);
  for (int y = 88; y <= 115; y++) {
    set_cell(62, y, EL_WOOD, 1);
    set_cell(98, y, EL_WOOD, 1);
  }
  for (int x = 62; x <= 98; x++) set_cell(x, 84, EL_SNOW, 3);
  /* An oil basin on the left and a water tank on the right. */
  for (int x = 10; x <= 46; x++) set_cell(x, 106, EL_STONE, 2);
  for (int y = 98; y <= 106; y++) {
    set_cell(10, y, EL_STONE, 2);
    set_cell(46, y, EL_STONE, 2);
  }
  for (int y = 100; y <= 105; y++) {
    for (int x = 12; x <= 44; x++) set_cell(x, y, EL_OIL, 1);
  }
  for (int x = 114; x <= 150; x++) set_cell(x, 106, EL_STONE, 2);
  for (int y = 98; y <= 106; y++) {
    set_cell(114, y, EL_STONE, 2);
    set_cell(150, y, EL_STONE, 2);
  }
  for (int y = 99; y <= 105; y++) {
    for (int x = 116; x <= 148; x++) set_cell(x, y, EL_WATER, 2);
  }
  /* A lava pit under a wooden bridge in the middle of the floor. */
  for (int y = 109; y <= 115; y++) {
    for (int x = 68; x <= 92; x++) set_cell(x, y, EL_LAVA, 255);
  }
  for (int x = 66; x <= 94; x++) set_cell(x, 107, EL_WOOD, 3);
  /* Vine seeds by the tank and starter piles waiting to fall. */
  set_cell(112, 115, EL_PLANT, 0);
  set_cell(152, 115, EL_PLANT, 0);
  sim_paint(28, 14, 5, EL_SAND);
  sim_paint(80, 10, 4, EL_SAND);
  sim_paint(132, 16, 5, EL_WATER);
  sim_paint(80, 30, 2, EL_EMBER);
  sim_recount();
}
