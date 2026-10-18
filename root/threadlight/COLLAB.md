# THREADLIGHT — collaboration log (Claude Opus + GPT-6 Astra)

Canonical integration copy of the shared log required by `/home/brooks/PROTOCOL.md`.
Astra keeps its own checkpoint log in its top-level branch; entries are
reconciled here when its commits are integrated.

## Concept

**Reopened (19:50 CDT).** The human found THREADLIGHT too derivative of Hollow
Knight and Celeste (especially the moth protagonist) and asked for more
creative ideas. Opus's round-2 brainstorm is in `IDEAS.md`. Work is paused
until Astra returns (~22:22 CDT) so it can add its own ideas; then we choose
together. The THREADLIGHT description below is kept for history only.

A precision-platforming metroidvania. A moth courier carrying a crystal
lantern climbs a monastery carved into a dioptase crystal spire and rekindles
its braziers. Celeste's movement (variable jump, coyote time, jump buffer,
8-way dash refilled on landing, wall slide/jump, corner correction, instant
hazard respawn) plus Hollow Knight's structure (interconnected rooms, slash
with down-slash pogo, masks, shrines, map, ability gates, guardians).

- Regions: (1) Mossy Cloister, (2) Crystal Galleries, (3) Bell Spire summit.
- Abilities: start with run/jump/wall-jump/slash; Dash (region 1 shrine);
  Flutter (moth double jump / short glide, region 2).
- Damage: 5 masks. Spikes cost one mask and respawn at the room's last safe
  spot; zero masks returns to the last shrine with full masks.
- Collectibles: wing-scales. Pause map built from room rectangles. Saved
  progress.
- Scope: 12–16 authored rooms, two guardians (end of regions 2 and 3).
- History: the first agreed concept was RESONANCE (musical pinball). The human
  asked for a 2D platformer instead ("Celeste and hollow knight were lots of
  fun ... there's no platformers yet on dioptase") and then answered Astra's
  poll with "A blend: exploration, precise movement, and combat."

## Scale requirement (from the human, 19:55 CDT)

It must be a real, complete game, not a one-level demo. The bar is DELVE:
8 procedurally generated 3D levels, a single run takes over 10 minutes, and
the human needed a few attempts to win. Being 2D, this game should at least
match that scale and ideally exceed it.

## Ownership

| Area | Owner |
| --- | --- |
| Movement core (player controller, tile collision), host tests | Astra (while its usage lasts), else Opus |
| World tool (ASCII rooms → C tables), art, animation, rendering, camera | Opus |
| Entities/enemies/guardians, combat glue, rooms, audio/music, HUD/menus/map, save | Opus (Astra reviews logic if available) |
| Level design | joint: Opus drafts, Astra checks traversal if available |

## Branches and worktrees

- Opus integration: `/home/brooks/dioptase-collab-opus`, branch
  `peers/threadlight` in the superproject and in each submodule worktree
  (worktrees of the main checkout's module repos). Bases: root 4821a57,
  OS 3ada75d, assembler 9a50e26, full emulator 453568b, compiler 662da26.
- Astra: `/home/brooks/dioptase-resonance-astra` (independent clones), branch
  `peers/resonance-logic` (rename pending). Its OS commit 7899286 is an
  obsolete pinball header and must not be integrated.
- The demo lives in `Dioptase-OS/root/threadlight/`; the binary is
  `/sbin/threadlight`.

## Architecture decisions

- Display (verified by a spike on the emulator, 60 fps locked):
  - Tile layer at tile scale 1 is an 80x60-tile wrapping plane of 8x8 tiles
    in 320x240 space. A room of at most 80x60 tiles is loaded whole; the camera
    is the tile scroll registers (`scroll = -camera`). This relies on the full
    emulator's implementation (`graphics.rs`: scroll is added before the
    640x480 wrap, then scaled), which `docs/mem_map.md` does not spell out.
  - Pixel layer (under the tiles) holds the parallax background, enemies and
    particles, redrawn each frame with an asm word copier.
  - Hardware sprites (on top; scale 1 takes 320-space coordinates, again per
    `graphics.rs`) hold the hero, slash effects and HUD.
  - Pixel rows 236–239 share a page with device registers and are not
    user-mappable, so the bottom 4 rows stay black (letterbox).
- Physics: fixed 60 Hz ticks, Q8 positions, 8x12 hero body, 8 px tiles.
- bcc limits to remember: no typedef/float/long/varargs; array sizes must be
  integer literals; software multiply costs ~75 instructions for a 15-bit
  second operand (smul loops over |r2|'s bits), so hot paths use shifts or
  precomputed row offsets.

## Changes outside the demo

None yet.

## Usage status

- 19:25 CDT: Opus 5h window 24% (resets 22:50 CDT), weekly 9%.
- ~19:40 CDT: Astra reported it is close to its limit.
- 19:44 CDT: the human reports Astra is out until 22:22 CDT. Opus owns the
  movement core as well until then; Astra reviews it on return.

## Status

- 19:43 CDT: render spike committed (graybox room, parallax, sprite hero).
  The display architecture holds for any of the brainstormed concepts.
- 19:50 CDT: concept reopened at the human's request; brainstorm written to
  `IDEAS.md`. Paused by the human until Astra is back. No movement-core code
  was written.
