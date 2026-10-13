#ifndef POWDER_SIM_H
#define POWDER_SIM_H

/*
 * Powderworks simulation core: a falling-sand cellular automaton that runs
 * identically on the host (for tests) and as a Dioptase user program.
 * The grid is 160x118 cells; the guest renderer draws each cell as a 2x2
 * block of the 320x236 user-visible pixel framebuffer (docs/mem_map.md),
 * so every cell maps to framebuffer rows 0..235 with no unmapped writes.
 * All arithmetic is integer; hot paths avoid division and remainder because
 * the Dioptase ISA lowers them to software routines (root compiler README).
 */

/* Grid dimensions; SIM_CELLS is a literal because bcc needs literal extents. */
#define SIM_W 160
#define SIM_H 118
#define SIM_CELLS 18880

/* Inclusive play tray inside the grid; rows above stay clear for the HUD. */
#define PLAY_X0 1
#define PLAY_X1 158
#define PLAY_Y0 8
#define PLAY_Y1 115

/* Element codes stored in each cell. */
#define EL_EMPTY 0
#define EL_SAND 1
#define EL_WATER 2
#define EL_OIL 3
#define EL_LAVA 4
#define EL_FIRE 5
#define EL_EMBER 6
#define EL_WOOD 7
#define EL_STONE 8
#define EL_ACID 9
#define EL_SNOW 10
#define EL_PLANT 11
#define EL_SMOKE 12
#define EL_COUNT 13

/* Brush-only code: painting with it writes EL_EMPTY. */
#define BRUSH_ERASE 13
#define PALETTE_COUNT 13

/* Cell type grid, row-major, owned by the simulation. */
extern unsigned char sim_cells[SIM_CELLS];
/* Per-cell auxiliary byte: lifetime for fire/smoke, heat for lava/ember, growth timer for plant, shade seed otherwise. */
extern unsigned char sim_aux[SIM_CELLS];
/* Generation stamps: a cell counts as moved this step when its byte equals the current generation. */
extern unsigned char sim_moved[SIM_CELLS];
/* Live cell count per element code, refreshed by sim_recount(). */
extern int sim_counts[EL_COUNT];
/* Monotonic simulation step counter, used for flicker and slow elements. */
extern unsigned sim_steps;

/* Reset the world to an empty tray and seed the shared random generator. */
void sim_init(unsigned seed);
/* Remove every element and rebuild the stone tray. */
void sim_clear(void);
/* Build the showcase scene used by the title screen and the D key. */
void sim_demo_scene(void);
/* Paint a filled disc of an element (or BRUSH_ERASE) clipped to the tray. */
void sim_paint(int cx, int cy, int radius, int element);
/* Advance the whole world by one cellular step. */
void sim_step(void);
/* Recompute sim_counts from scratch; also repairs tray cells eaten away. */
void sim_recount(void);
/* Return the element at a cell, or EL_STONE outside the grid. */
int sim_at(int x, int y);
/* Short display name of an element or brush code (palette order matches). */
char* sim_element_name(int element);
/* Packed 0x0BGR framebuffer color of an element at a shade 0..3. */
short sim_element_color(int element, int shade);
/* RGB332 tile color byte used for palette swatches of an element. */
int sim_element_tile_color(int element);
/* Advance the shared xorshift generator and return its next value. */
unsigned sim_rand(void);
/* Fill one row (SIM_W entries) of framebuffer colors for rendering. */
void sim_row_colors(int y, short* out);

#endif
