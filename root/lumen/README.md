# LUMEN: The Glass Station

A first-person expedition through a silent orbital station. Restore its three
colored relays, outwit the sentries, and bring the last sun home.

LUMEN is an original user program for Dioptase: a textured integer raycaster,
three procedural decks, tactical combat, a scanner map, crew transmissions,
upgrades, and an original synthesized score. No external artwork, music files,
engine or host graphics library is needed by the game. Its menu is signed
**gpt-6-astra**.

![LUMEN running as a Dioptase user program](screenshots/game.png)

## Play

From this checkout:

```sh
cd Dioptase-OS
make run EMU_VGA=yes
```

At the guest shell, enter:

```text
lumen
```

Press **Enter** to play or **D** on the title screen to watch an autonomous
expedition. Enter first shows a mission briefing; press Enter again to begin.
A persistent green objective line keeps the next step visible during play.

Turning uses fine 11.25-degree increments instead of quarter-turns. Movement
takes one cell at a time in the nearest of eight directions, with animated
transitions; your aim stays at the angle you selected. Diagonal steps need
both adjoining cells clear. Beams follow your aim, with forgiving cell-sized
hitboxes for relays and sentries.
Enemies act when you move, fire, wait, open a door, or read a transmission.
You can take your time looking, changing colors, and studying the map.

| Key | Action |
| --- | --- |
| W / S, Up / Down | Forward / backward |
| A / D, Left / Right | Aim left / right |
| Z / C | Sidestep left / right |
| Space or F | Fire the prism beam |
| 1 / 2 / 3 | Cyan / amber / magenta beam |
| E or Enter | Open door, read transmission, use lift |
| X or period | Wait and recharge |
| M or Tab | Scanner map |
| H | Field manual |
| P | Pause |
| V | Toggle sound |
| T | Toggle autonomous pilot; movement also takes control |
| R / N | After an ending: retry the seed / start a new seed |
| Q | Return to the shell |

The emulator itself handles Escape as **close window**, so use Q to leave the
game while keeping the OS running.

Match each relay's glow with your beam. A matching shot also does much more
damage to a sentry. Its **!** warning gives you two turns to destroy it or break
its line of fire; sidestepping behind a corner cancels its lock. Shots cost two
charges. Waiting recharges, but enemies take their turns too. Medical caches,
power cells, and crew logs help you survive.

Three lit relays unlock the lift in the southeast chamber. Look for its glowing
arch, or the green map square. Face it from the adjacent cell (or stand on it)
and press E. Between decks, choose hull, beam, or capacitor upgrades. Restore
all three decks to finish the expedition.

## Options

```text
lumen --seed 42
lumen --demo
lumen --silent
```

The default seed is `20261010`; `--seed` accepts decimal digits and wraps to a
32-bit unsigned value. There are no saved-game or asset-file dependencies.

Sound uses the existing 25 kHz mono audio device. `make run EMU_VGA=yes` selects
the emulator's wall-clock audio mode; its host playback requires the audio
player described in the emulator README. To run without host playback:

```sh
make run EMU_VGA=yes EMU_AUDIO=no EMU_AUDIO_FAST=no
```

Then use `lumen --silent`. If another process owns audio, the game explains the
failure in its radio strip and remains playable.

## Build and verification

The OS discovers `root/lumen/Makefile` automatically. To build only the game:

```sh
make -C root/lumen
```

Run the actual optimized guest binary, including deterministic rule tests and
seven scenes published to and read back from the real tile/sprite mappings:

```sh
make lumen.test TEST_RUNS=1 TIMEOUT_SECONDS=600 EMU_VGA=no EMU_AUDIO=no
```

The test must report `1/1`. The existing OS test runner does not propagate a
failing summary through its exit status. Its output directory contains guest
PPM captures; compare these pixel-for-pixel against a native build and create
viewable PNGs with:

```sh
make -C root/lumen previews
```

Portable checks and optional host sanitizers:

```sh
make -C root/lumen host-check
make -C root/lumen host-sanitize
root/lumen/build/check --exhaustive
```

The host checks exercise deterministic generation, complete campaigns,
eight-way movement, corner collision, oblique beams, relay idempotence, sentry
warning/dodge/death, resource limits, logs, upgrades, every UI mode, extreme views,
wall occlusion, HUD/projection cache invalidation, and PCM synthesis. The guest
self-test additionally checks briefing input, queued movement, repeated turns,
and camera timing across frame-counter wrap. `--exhaustive` runs 1,000 complete
campaigns.
The integration test executes the same game, renderer, and synth with bcc and
basm under the full emulator; host checks alone are not evidence of guest
compiler correctness.

## Implementation contracts

- All game logic runs in a single foreground user process. There are no worker
  threads, custom traps, privileged instructions, or assumptions of POSIX libc.
- [The root ABI specification](../../../docs/abi.md) supplies 16-bit short and 32-bit int, the calling convention,
  four-byte stack alignment, and the CRT trap ABI. No TLS is used.
- [The root memory map](../../../docs/mem_map.md) defines tile entries, `0x0BGR` color, 256 writable 8x8 tiles,
  and 16 writable 32x32 sprites. 240 tiles form a 160x96 view enlarged to
  640x384; ten sprites form the sharper 320x32 radio/status strip.
- The canonical memory map does not specify whether sprite coordinates scale
  with their pixels. This application explicitly follows the current emulator
  and `root/dino` behavior: both scale together. The missing coordinate rule
  should be added to the canonical hardware documentation before targeting a
  different implementation. The canonical mouse-stream section is also absent;
  this application uses keyboard input only.
- The mapping getters and audio ownership/error/partial-write rules come from
  [the OS syscall documentation](../../docs/syscalls.md). PCM remains in an application buffer until its accepted
  prefix is reported; no unwritten suffix is discarded.
- The terminal owns raw keyboard input and forwards press bytes through stdin.
  LUMEN consumes that pipe, falling back to `getkey()` only without a pipe.
  It hides the cursor and drains terminal output before taking over graphics.
  The shell restores its display after the foreground program exits.
- Writes use volatile mapped pointers and stay within each documented region.
  Packed pixel publication uses the current emulator's little-endian word
  stores. General word-store endianness is unspecified in the ISA text; this
  dependency is verified through actual guest MMIO readbacks.
  The display has no documented atomic buffer swap; a frame can briefly tear
  during publication. Animation is paced by the VGA counter, which requires
  `--vga` or `--headless` in the emulator. Gameplay remains turn based at any rendering speed.
- The sample loop uses fixed-point lookup tables because multiplication itself
  is a software routine on this ISA. The original score, **Afterlight**, has
  three deck arrangements and synthesized cues; all visuals are procedural.

## Performance

The renderer can publish on every VGA tick instead of imposing a 15 fps cap.
Camera interpolation uses elapsed ticks, so animation duration does not depend
on rendering throughput. Direction-dependent projection arithmetic is cached,
unchanged HUD content is retained, and aligned word stores publish two VGA
pixels at once. The seven guest captures check the resulting pixels against
the native renderer, including the new briefing and an oblique view.

This branch also adapts Claude's OS optimizations from `claude-demo` commits
`ae850820` (per-core/queue arithmetic) and `29156c76` (empty pending-signal
fast path), credited upstream to Brooks Bryant and Claude Opus 5.5. The queue
walk preserves nested C array bounds. Emulator commit `6edcc5e` reduces VGA
lock contention and is already present in the tested `claude-demo` emulator.
No new graphics syscalls are required.

For a finite, uncapped rendering benchmark without audio, run:

```text
lumen --benchmark
```

Use a VGA window or the emulator's `--headless` mode so the 60 Hz counter
advances. The benchmark renders 96 frames each of a stationary view, camera
translation, and camera rotation, including real MMIO publication. It reports
VGA ticks; frames per second are `96 * 60 / ticks`. Startup and console output
are excluded. This measures rendering throughput; live audio and gameplay can
add work. On the same four-core full emulator, the original renderer measured
about 24 fps uncapped, while the final renderer measured about 43–52 fps
(111/110/135 ticks versus 233/236/234 for stationary/moving/turning views).
A second final run measured 110/111/135 ticks. One baseline repeat was slower
(621/619/619 ticks); these are host wall-clock measurements, not ISA cycle counts.
Actual gameplay previously also had a 15 fps cap; it now targets each VGA tick.


## Verified follow-up

- Optimized guest `lumen.test`: **1/1**, including input and gameplay self-tests.
- Seven final guest MMIO captures match the native renderer pixel-for-pixel.
- Native checks and ASan/UBSan: **8,334 frames and 128 campaigns** each.
  The exhaustive sweep passed **1,000 campaigns** and **17,550 frames**.
- Six selected OS regressions passed **3/3 before and after** the kernel changes:
  `user_kill`, `user_foreground_display`, `user_syscall_invalid_ptr`,
  `simple_fork`, `user_fork_wait_mmap_sem`, and `threads_race_stress`.
- `root-sbin` builds, and a headless keyboard script launched Lumen through the
  real terminal/shell, entered the briefing, turned, moved, opened help/map,
  and returned to the shell. Audio capture contains nonzero bounded PCM;
  no audible listening test was performed.
