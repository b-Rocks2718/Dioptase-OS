# MICROREALM: Bastion of Antigravity

**Created by Antigravity**

A real-time strategy, colony survival, and castle defense game created specifically for the Dioptase custom architecture, operating system, and hardware stack.

---

## Overview

In **MicroRealm: Bastion of Antigravity**, you command a fledgling settlement founded upon a wild frontier island. By dawn and day, your citizens harvest lumber from dense forests, quarry stone, mine glittering gold veins, plant wheat farms, and construct a fortified stronghold.

As dusk descends, warning bells toll across the valleys. When night falls and the blood moon rises, hordes of goblins, orc marauders, skeletal archers, siege ogres, and shadowy fiends assault your walls. Your watchtowers rain down volleys of arrows, and armored swordsmen clash shields on the ramparts to defend the royal Castle.

Survive consecutive days and nights to expand your kingdom, upgrade your armies, and achieve legendary scores recorded persistently to SD disk.

---

## Architectural & System Features

MicroRealm is designed from the ground up to showcase the unique capabilities of the Dioptase computer architecture:

- **Direct Pixel Framebuffer Compositing (`get_pixel_fb`)**:
  Renders a 64x64 tile procedural world (1024x1024 world pixels) into Dioptase's 320x240 12-bit RGB (`0x0BGR`) direct framebuffer. Features smooth pixel scrolling, animated humanoid walk cycles, attack flourishes, directional facing, dynamic health gauges, particle sparks, blood bursts, and floating combat damage text.

- **Dual-Layer Tactical HUD & Minimap**:
  Rows 208..235 host an integrated command console. The lower-left radar displays a real-time minimap tracking the island coastline, friendly buildings, player units, invading hostiles, and the current camera viewport. Players can click anywhere on the radar to instantly pan the camera.

- **Full PS/2 Mouse Integration (`getmouse`)**:
  Decodes raw PS/2 mouse packets with subpixel sensitivity. Supports click-and-drag bounding box selection for commanding entire squads, right-click contextual orders (gather, move, attack), building placement ghost previews (green when valid, red when obstructed), and minimap panning.

- **Real-Time 25 kHz PCM Audio Synthesizer (`audio_write`)**:
  Streams signed 16-bit little-endian mono PCM directly to the audio ring buffer DMA. Features a multi-channel software audio mixer generating a lively Renaissance Dorian background melody that shifts dynamically during nighttime raids, alongside procedural sound effects: axe chopping, pickaxe mining, bow releases, sword clashing, building placement thuds, dusk cathedral alarms, and dawn victory fanfares.

- **Autonomous AI Commander ("Auto-Play" Mode)**:
  Press **D** at any time (or launch with `--demo`) to activate the built-in AI Governor. The autonomous agent manages the kingdom's economy, balances wood/stone/gold extraction, expands housing before population caps, places defensive watchtowers along borders, constructs barracks, trains swordsmen and archers, and maneuvers armies to repel night raids.

- **Persistent High-Score Ledger (`/microrealm_save.dat`)**:
  Saves your best campaign survival record (Days Survived, Enemies Slain, High Score) directly to the ext2 SD filesystem across boots.

- **Deterministic Headless Guest Engine Check (`--selftest`)**:
  Runs a comprehensive automated test exercising world generation, building placement, unit production, pathfinding, 110 simulation ticks, combat resolution, and record persistence. Fully integrated into the test suite via `make microrealm_guest.summary-test`.

---

## How to Play

### 1. Launching from the Dioptase Shell

Boot the Dioptase OS emulator with graphical VGA and audio enabled:

```sh
cd Dioptase-OS
make run EMU_VGA=yes
```

At the Dioptase shell prompt, enter:

```text
microrealm
```

To launch directly into autonomous spectator mode:

```text
microrealm --demo
```

### 2. Controls

| Control | Action |
| :--- | :--- |
| **Left-Click** | Select unit / Click building to train / Confirm structure placement |
| **Left-Drag** | Drag bounding box to select multiple military units or villagers |
| **Right-Click** | Issue move command, order villager to harvest resource, or cancel placement |
| **Minimap Click** | Click or drag radar on bottom-left to pan the camera across the island |
| **WASD / Arrows** | Scroll camera across the world (also supports edge scrolling) |
| **H** | Select **House** for construction (40 Wood, +5 Population capacity) |
| **F** | Select **Farm** for construction (30 Wood, yields continuous Food) |
| **T** | Select **Guard Tower** for construction (50 Wood, 35 Stone, automated archery) |
| **B** | Select **Barracks** for construction (70 Wood, 40 Stone, trains military) |
| **W** | Select **Stone Wall** for construction (15 Stone, blocks enemy passage) |
| **V** | Train **Villager** at Castle (40 Food) |
| **S** | Train **Swordsman** at Barracks (50 Food, 25 Gold) |
| **A** | Train **Archer** at Barracks (45 Food, 30 Gold) |
| **D** | Toggle **Autonomous AI Commander** on / off |
| **M** | Toggle Sound / Music on / off |
| **Space** | Pause / Resume gameplay |
| **Q** | Cleanly exit to the Dioptase shell |

---

## Economy & Defense Strategy

1. **Morning Gathering**: Assign starting villagers to chop trees and mine stone. Wood is essential for houses and towers.
2. **Expansion**: Build a **House** before hitting the 10-population cap. Construct a **Farm** to ensure a steady supply of Food for training recruits.
3. **Defense Preparation**: Erect **Guard Towers** near chokepoints and vulnerable approaches before the Dusk bell tolls. Towers fire automatically at any monsters entering their perimeter.
4. **Barracks & Army**: Once Stone and Gold reserves allow, build a **Barracks** and recruit **Swordsmen** to hold the frontline and **Archers** to provide covering fire.
5. **Night Defense**: When night falls, group your soldiers near the Castle or towers. Protect your villagers—if your Castle falls, the realm is lost!


## Performance checks

The renderer fills clipped rectangles eight pixels at a time, retaining 16-bit
framebuffer accesses. The audio mixer refills toward 6,000 queued bytes (120 ms
at 25 kHz) independently of the presentation rate, retaining PCM across partial
or temporarily blocked writes.

Run the engine and media regressions from `Dioptase-OS`:

```sh
make microrealm_guest.summary-test microrealm_media_guest.summary-test TEST_RUNS=3
```

The media guest compiles the actual renderer and synthesizer with deterministic
device doubles. It checks an eight-frame scene hash, full-range signed decimal
text, clipping guards, transparent glyphs, transparent tile overlay, failed display mappings, audio refill,
and PCM preservation across partial writes. The engine guest checks campaign
reset and midnight harvest timing as well as its existing simulation checks.

For a fixed rendering profile, compile the media guest with
`OS_BCC_DEFINES='-DOS_RELEASE=1 -DMEDIA_BENCHMARK'`, package its
`tests/microrealm_media_guest.dir` directory as an ext2 root, and run the full
emulator with `--profile`, `--profile-start user`, `--profile-stop stop`, and
`--user-symbols tests/microrealm_media_guest.dir/sbin/build/init.hex`.
`MEDIA_BENCHMARK` skips the additional edge-case checks; normal test builds
always enable them. The Makefile tracks flag changes to prevent stale binaries.

On the recovered Gemini source, rectangle filling consumed 6,282,734 dispatched
instructions for this workload; the completed version consumed 3,123,220
(50.3% fewer), with the same `10517ae7` frame hash. These are emulator instruction
counts, not hardware cycles or an FPS claim; the checksum loop itself is excluded
from this rectangle-filling comparison.


## Display startup

The terminal's tile layer is drawn above the pixel framebuffer. Clearing the
terminal with `CSI 2J` selects its opaque black tile and does not expose a pixel
game. After the terminal handoff, MicroRealm initializes transparent tile 255
and selects it throughout the documented 80x60 tile framebuffer. It checks the
pixel, tile framebuffer, and tilemap mappings before changing display contents.
If a mapping fails, startup restores terminal display state and reports the
failed graphics operation instead of continuing with invisible rendering.

The expanded media test starts with opaque tile graphics and verifies every
selected tile pixel is transparent, along with display scale, scrolling, and
mapping failure paths. A real headless run through init/terminal/shell also
verified the title, Enter to start, D to enable autoplay, and Q to restore the
shell. The scripted sequence is in `tools/display-smoke.input`; pass it to the
full emulator with `--headless --input-script` and a `--shot-dir` after building
and packaging the normal root filesystem.
