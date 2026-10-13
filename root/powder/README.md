# POWDERWORKS — a falling-sand alchemy lab

![Powderworks running on the Dioptase emulator](preview.png)

Paint living matter into a stone tray and watch physics argue with itself.
Sand piles and sinks, water flows and quenches, oil floats and burns, vines
drink and climb. There is no score and no losing: it is a toy box, and the
fun is in the mixing.

Made by **Muse Code — powered by Meta Muse Spark**, whose name is also on
the in-game title screen and handbook.

## Play

From the `Dioptase-OS` directory, with the toolchain built:

```sh
make run EMU_VGA=yes EMU_AUDIO_FAST=yes
```

At the guest shell, enter `powder`. The title screen plays itself behind the
menu (an attract painter rains elements onto the showcase scene); press
Enter or click to take the brush. Audio is optional; use
`EMU_AUDIO=no EMU_AUDIO_FAST=no` and `powder --silent` without a host player.

| Input | Action |
| --- | --- |
| mouse | move the brush (hardware sprite ring) |
| left button | paint the selected element |
| right button | erase |
| middle button | sample the element under the brush |
| wheel / B | brush size |
| `1`–`9`, `0`, `[`, `]` | choose element |
| `V` / `G` / `X` | vine / smoke / eraser |
| WASD / arrows + Space | keyboard brush + paint |
| `P` | pause |
| `C` | clear the tray |
| `N` | rebuild the showcase scene (burn the bridge!) |
| `O` / `L` | save / load `/powder/scene.pwd` |
| `H` | handbook |
| `M` | sound on/off |
| `Q` | return to the shell |

## The elements

SAND sinks through water · OIL floats on water and catches fire · LAVA meets
WATER and quenches to STONE with a hiss of steam · FIRE eats WOOD, OIL and
VINE, then dies to SMOKE · EMBERs fall hot, light what they touch, and cool
to ash · ACID dissolves sand, wood, vine and even stone, spending itself as
smoke · SNOW melts beside heat · VINE grows toward any nearby water · SMOKE
rises and thins away.

Options: `powder --demo` skips the title and leaves the attract painter on;
`powder --silent` never opens the audio device; `powder --selftest` runs the
deterministic guest check and prints `***PASS powder guest engine`;
`powder --bench` times 300 simulation steps (with and without rendering) on
a fixed scene in guest jiffies and shows the results until Q. On the current
build the bench reads about 5,200 jiffies for the steps and 9,200 for steps
plus rendering, against 9,400 and 17,400 for the original build: the hot
passes walk flat cell indices instead of calling coordinate helpers, moved
cells carry a generation stamp instead of a cleared flag array, row colors
are computed in one call-free pass over a cached static background, and the
framebuffer publish is the handwritten `powder_publish_row` loop in
`powder_asm.s`, which packs each 2x2 cell into one word store per row.
There are no multiply, divide, or remainder operations left in any
per-step or per-frame path (the compiler lowers them to runtime calls);
the only survivors are in the brush dab and the HUD number printing.

## Checks

Host rules (pure simulation, no emulator):

```sh
make -C root/powder test
```

Full-emulator guest check (exec, rules, display mappings, audio, save/load):

```sh
make powder_guest.summary-test TEST_RUNS=1
```

## Device contracts

User mode only, via documented Dioptase-OS syscalls. The world is the pixel
framebuffer from `get_pixel_fb()` (320x236 user pixels, `0x0BGR`, two frame
rows per cell row); HUD text and the palette are tile-framebuffer entries
over text tiles loaded with `load_text_tiles()` (tile 255 transparent, tile
127 the solid swatch); the brush is sprite 0 via `get_spritemap()` and
`set_sprite_coords()`. Mouse events come from `getmouse()` decoded with
`root/crt/mouse.h`; keyboard uses `set_keyboard_raw(1)` two-byte records
with a cooked-byte and `getkey()` fallback. Sound effects are synthesized
PCM written non-blocking to the 25 kHz audio device by its owner process.
Scenes are the raw 160x118 cell grid behind a 16-byte `PW` header.
