# DELVE — Into the Dioptase Deep

A real-time, first-person dungeon crawler for the Dioptase computer: textured
3D corridors, flickering torchlight, monsters, spells you aim with the mouse,
procedurally generated levels, a boss, chiptune music, and a high-score table
saved to disk. Everything — code, art, music — is generated or written for
this machine; the game runs as an ordinary user program on Dioptase-OS.

## Running it

From `Dioptase-OS/`:

```sh
make run EMU_VGA=yes
```

then type `delve` at the shell prompt. `make run` builds every program under
`root/`, including this one, into `root/sbin/`.

## How to play

You are a crystal-hunter descending through the eight depths of an ancient
mine. On each depth:

1. Gather the green **dioptase crystals** (score).
2. Find the **rune key**.
3. Carry it to the **sealed door** (the iron door with the gold lock) and
   step through the swirling **portal** behind it to descend.

On depth 8 the key is carried by **the Warden**. Defeat it, break the seal,
and take the **Heart of Dioptase** to win.

| Keys | Action |
| --- | --- |
| `W` `S` / up, down | step forward / back |
| `Shift+W` | run ahead until a junction, door, or monster |
| `A` `D` | step left / right |
| `Q` `E` / left, right | turn |
| mouse | aim (the crosshair) |
| left button / `Space` | spark bolt (cheap, fast; `Space` fires straight ahead) |
| right button / `F` | fireball (costly, slow, splash damage) |
| `M` or `Tab` | automap |
| `P` | pause (`X` from the pause panel gives up) |
| `H` (title screen) | how to play |
| `X` (title screen) | quit to the shell |

Mana regenerates over time; red potions restore health, blue ones mana.
Monsters wake when you come near: slimes and bats bite, floating eyes spit
bolts you can sidestep, and wraiths hit hard. Ctrl-C quits at any time.

Cheats, for exploring the deep levels quickly: `Shift+G` toggles invulnerable
mode, `=` takes the portal immediately, `\` hands the controls to the
autopilot, and `` ` `` shows the frame rate.

Why steps instead of smooth walking? The terminal forwards key *presses* to the
foreground program but never key releases, so a program run from the shell
cannot tell how long a key is held. DELVE is designed around that: every key
press is one grid step or a quarter turn (animated smoothly), and the mouse —
whose button state the program reads directly — handles aiming and casting.

## How it works

**A framebuffer made of tiles.** User programs can map the tile layer and the
sprites but not the pixel framebuffer. DELVE switches the tile layer to 4x
scale, where a 20x12 grid of cells covers the top 640x384 screen pixels, and
points those 240 cells at 240 *distinct* tiles. Rewriting the tiles' pixel data
then acts as a 160x96, 12-bit color framebuffer. The tiles are assigned
column-major (cell (tx, ty) shows tile tx * 12 + ty), so a screen column is a
constant 16-byte stride through tile memory and a finished frame is one
straight 30 KiB copy from the back buffer. The status bar below is five 32x32
hardware sprites at scale 2; the staff and crosshair are more sprites.

**Raycasting.** One DDA ray per screen column finds walls and Wolfenstein-style
sliding doors (thin panels on a cell's center plane). Per-angle ray tables are
cached, since the camera only turns during the quarter-turn animation. Floors
and ceilings use row-based floor casting (texture coordinates step linearly
along each screen row). Billboards (monsters,
items, torches, spells) are depth-sorted and clipped against a per-column
z-buffer. Everything is lit through 16 shade tables over a 256-color palette:
distance fog plus point lights (torches, spells, crystals, the portal) that
light walls per column and floors per 8-pixel run.

**No multiply instruction.** The ISA has no hardware multiply, so the hot paths
use a quarter-square table multiplier (`umul16`), a shift/subtract divider,
and incremental arithmetic; the per-pixel loops are hand-written assembly in
`gfx.s`. The floor caster walks the screen one 8-pixel tile column at a time
because walking whole rows touches eight pages per row and thrashes the
16-entry TLB.

**Sound.** `audio_start()` forks a synthesizer process that owns the PCM
device: eight voices (wavetables and an LFSR noise channel), a four-channel
tracker for the music, and pitch-swept segment programs for the effects. The
game sends it one-byte commands through a pipe. A heartbeat and a
`SIGNAL_TERMINATE` handler make sure it never outlives the game.

**Attract mode.** Behind the title screen an autopilot plays the game: it
breadth-first searches toward potions, keys, crystals, the seal, and
unexplored space, and turns to fire at monsters in line with it.

**Persistence.** High scores live in `/delve.hi` on the root filesystem;
`make run` copies the image back into `root/`, so they survive between runs.

## Files

| File | Contents |
| --- | --- |
| `delve.h` | shared constants, types, and the view/tile layout |
| `main.c` | display setup, main loop, input, player, game states, high scores |
| `render.c` | raycaster, floor casting, lighting, billboards, particles |
| `world.c` | level generator, doors, monsters and AI, items, projectiles |
| `hud.c` | fonts, text, status bar upload |
| `audio.c` | synthesizer process and its command protocol |
| `autopilot.c` | the computer player for the attract mode |
| `gfx.s` | hand-written inner loops and arithmetic helpers |
| `assets.s` | generated palette, shade tables, textures, sprites, fonts, music |
| `tools/gen_assets.py` | the generator for `assets.s` (`make assets`; needs numpy) |

`python3 tools/gen_assets.py assets.s --preview <dir>` also writes PNG contact
sheets of the palette, textures, and sprites.
