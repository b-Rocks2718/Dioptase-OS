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

/* Swap two cells by flat index and mark both as moved this step. */
static void swap_cells(int a, int b) {
  unsigned char t = sim_cells[a];
  sim_cells[a] = sim_cells[b];
  sim_cells[b] = t;
  t = sim_aux[a];
  sim_aux[a] = sim_aux[b];
  sim_aux[b] = t;
  sim_moved[a] = 1;
  sim_moved[b] = 1;
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

/* Move the cell at (x, y) down, down-diagonally, in a random diagonal order. */
static int fall_down(int x, int y) {
  int i = cell_index(x, y);
  int below = sim_at(x, y + 1);
  if (fall_target_ok(below)) {
    swap_cells(i, i + SIM_W);
    return 1;
  }
  int dir = (sim_rand() & 1) != 0 ? 1 : -1;
  for (int k = 0; k < 2; k++) {
    int nx = x + dir;
    if (nx >= 0 && nx < SIM_W && fall_target_ok(sim_at(nx, y + 1)) &&
        fall_target_ok(sim_at(nx, y))) {
      swap_cells(i, cell_index(nx, y + 1));
      return 1;
    }
    dir = -dir;
  }
  return 0;
}

/* Spread the liquid at (x, y) sideways toward the first open cell. */
static int flow_sideways(int x, int y, int reach) {
  int i = cell_index(x, y);
  int dir = (sim_rand() & 1) != 0 ? 1 : -1;
  for (int k = 0; k < 2; k++) {
    for (int d = 1; d <= reach; d++) {
      int nx = x + dir * d;
      if (nx < 0 || nx >= SIM_W) break;
      if (!fall_target_ok(sim_at(nx, y))) break;
      if (d == reach || fall_target_ok(sim_at(nx, y + 1))) {
        swap_cells(i, cell_index(nx, y));
        return 1;
      }
    }
    dir = -dir;
  }
  return 0;
}

/* Ignite a flammable cell next to heat with probability 1 in (mask+1). */
static void try_ignite(int x, int y, int mask) {
  int t = sim_at(x, y);
  if (t != EL_WOOD && t != EL_OIL && t != EL_PLANT) return;
  if ((sim_rand() & (unsigned)mask) == 0) {
    set_cell(x, y, EL_FIRE, 25 + (int)(sim_rand() & 15));
  }
}

/* Advance falling and flowing cells; scans bottom-up so piles settle in one pass. */
static void step_fallers(void) {
  for (int y = SIM_H - 2; y >= 0; y--) {
    int left_first = ((sim_steps + (unsigned)y) & 1) == 0;
    for (int s = 0; s < SIM_W; s++) {
      int x = left_first ? s : SIM_W - 1 - s;
      int i = cell_index(x, y);
      int t = sim_cells[i];
      if (t == EL_EMPTY || sim_moved[i]) continue;
      if (t == EL_SAND || t == EL_EMBER) {
        int below = sim_at(x, y + 1);
        if (t == EL_SAND &&
            (below == EL_WATER || below == EL_OIL || below == EL_ACID)) {
          swap_cells(i, i + SIM_W); /* Sand sinks through lighter liquids. */
        } else {
          fall_down(x, y);
        }
      } else if (t == EL_WATER || t == EL_ACID) {
        if (t == EL_WATER && sim_at(x, y + 1) == EL_OIL) {
          swap_cells(i, i + SIM_W); /* Water sinks under oil, so oil floats. */
        } else if (!fall_down(x, y)) {
          flow_sideways(x, y, 3);
        }
      } else if (t == EL_OIL) {
        if (!fall_down(x, y)) flow_sideways(x, y, 2);
      } else if (t == EL_LAVA) {
        /* Lava is thick: it only moves on alternating steps. */
        if (((x + (int)sim_steps) & 1) == 0 && !fall_down(x, y)) {
          flow_sideways(x, y, 1);
        }
      } else if (t == EL_SNOW) {
        /* Snow falls at half rate, which makes soft, steep piles. */
        if (((x + y + (int)sim_steps) & 1) == 0) fall_down(x, y);
      }
    }
  }
}

/* Advance rising cells and every reaction that needs neighbouring heat. */
static void step_risers_and_reactions(void) {
  for (int y = 0; y < SIM_H - 1; y++) {
    for (int x = 0; x < SIM_W; x++) {
      int i = cell_index(x, y);
      int t = sim_cells[i];
      if (t == EL_FIRE) {
        if (sim_aux[i] > 0) sim_aux[i]--;
        try_ignite(x + 1, y, 15);
        try_ignite(x - 1, y, 15);
        try_ignite(x, y + 1, 15);
        try_ignite(x, y - 1, 15);
        if (sim_aux[i] == 0) {
          if ((sim_rand() & 3) == 0) set_cell(x, y, EL_SMOKE, 30);
          else set_cell(x, y, EL_EMPTY, 0);
        } else if (!sim_moved[i]) {
          /* Flames climb into empty space and shed smoke as they go. */
          if (sim_at(x, y - 1) == EL_EMPTY && (sim_rand() & 3) == 0) {
            set_cell(x, y - 1, EL_SMOKE, 25 + (int)(sim_rand() & 15));
          }
          if (fall_target_ok(sim_at(x, y - 1)) && sim_at(x, y - 1) != EL_EMPTY) {
            swap_cells(i, i - SIM_W);
          } else if (sim_at(x, y - 1) == EL_EMPTY && (sim_rand() & 1) == 0) {
            swap_cells(i, i - SIM_W);
          }
        }
      } else if (t == EL_SMOKE) {
        if (sim_aux[i] > 0) sim_aux[i]--;
        if (sim_aux[i] == 0) {
          set_cell(x, y, EL_EMPTY, 0);
        } else if (!sim_moved[i]) {
          if (sim_at(x, y - 1) == EL_EMPTY) {
            swap_cells(i, i - SIM_W);
          } else {
            flow_sideways(x, y, 1);
          }
        }
      } else if (t == EL_LAVA) {
        /* Lava quenches to stone in water and lights what it touches. */
        if (sim_at(x + 1, y) == EL_WATER || sim_at(x - 1, y) == EL_WATER ||
            sim_at(x, y + 1) == EL_WATER || sim_at(x, y - 1) == EL_WATER) {
          set_cell(x, y, EL_STONE, 1);
          if (sim_at(x, y - 1) == EL_EMPTY) {
            set_cell(x, y - 1, EL_SMOKE, 40); /* The hiss becomes steam. */
          }
        } else {
          try_ignite(x + 1, y, 31);
          try_ignite(x - 1, y, 31);
          try_ignite(x, y + 1, 31);
          try_ignite(x, y - 1, 31);
        }
      } else if (t == EL_EMBER) {
        try_ignite(x + 1, y, 31);
        try_ignite(x - 1, y, 31);
        try_ignite(x, y + 1, 31);
        if (sim_aux[i] > 0) sim_aux[i]--;
        if (sim_aux[i] == 0) set_cell(x, y, EL_STONE, 0); /* A spent ember is ash. */
      } else if (t == EL_ACID) {
        /* Acid eats one neighbouring solid, then spends itself as smoke. */
        int eaten = 0;
        if (sim_at(x, y + 1) == EL_SAND || sim_at(x, y + 1) == EL_WOOD ||
            sim_at(x, y + 1) == EL_PLANT) {
          eaten = cell_index(x, y + 1);
        } else if (sim_at(x + 1, y) == EL_STONE || sim_at(x - 1, y) == EL_STONE) {
          eaten = cell_index(x + (sim_at(x + 1, y) == EL_STONE ? 1 : -1), y);
        }
        if (eaten != 0 && (sim_rand() & 7) == 0) {
          sim_cells[eaten] = EL_EMPTY;
          sim_aux[eaten] = 0;
          set_cell(x, y, EL_SMOKE, 20);
        }
      } else if (t == EL_PLANT) {
        /* Vines climb into empty cells while any surrounding cell holds water. */
        int fed = sim_at(x + 1, y) == EL_WATER || sim_at(x - 1, y) == EL_WATER ||
                  sim_at(x, y + 1) == EL_WATER || sim_at(x, y - 1) == EL_WATER ||
                  sim_at(x + 1, y + 1) == EL_WATER || sim_at(x - 1, y + 1) == EL_WATER ||
                  sim_at(x + 1, y - 1) == EL_WATER || sim_at(x - 1, y - 1) == EL_WATER;
        if (fed && (sim_rand() & 63) == 0) {
          int dir = (sim_rand() & 1) != 0 ? 1 : -1;
          if (sim_at(x, y - 1) == EL_EMPTY) set_cell(x, y - 1, EL_PLANT, 0);
          else if (sim_at(x + dir, y) == EL_EMPTY) set_cell(x + dir, y, EL_PLANT, 0);
        }
      } else if (t == EL_SNOW) {
        /* Snow touching heat melts where it lies. */
        if (sim_at(x + 1, y) == EL_FIRE || sim_at(x - 1, y) == EL_LAVA ||
            sim_at(x, y + 1) == EL_LAVA || sim_at(x, y - 1) == EL_FIRE ||
            sim_at(x, y + 1) == EL_EMBER) {
          set_cell(x, y, EL_WATER, 1);
        }
      }
    }
  }
}

/* Advance the whole world by one cellular step. */
void sim_step(void) {
  for (int i = 0; i < SIM_CELLS; i++) sim_moved[i] = 0;
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
