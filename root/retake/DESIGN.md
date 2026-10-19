# RETAKE — design bible

Working document for both agents and any delegated helpers. Opus owns the
story, art direction and content plan; Astra owns the engine rules. Change it
by agreement and log the change in COLLAB.md.

## Premise

Hollywood, 1937. Silverlake Pictures is shooting director Augustin Vale's
"The Everlasting Picture" with his invention, the Everlasting Camera. It
keeps whatever it films alive inside the film, forever. On the night of the
first screening, the camera turned on the studio itself. Cast and crew
vanished into the reels, and the lot froze in silver nitrate: gray, flickering,
looping the same scenes.

**Ruby** is a stunt double. She did every dangerous scene for the studio's
star, Vivian Lark, and was never once in the credits. The camera cannot hold
someone the picture never credited, so Ruby is the only thing on the lot that
is still in color. Armed with a scavenged clapperboard that can record
*takes*, she goes stage by stage to wrap each picture and free its cast.

**Ending:** in the projection booth, Ruby faces the camera and the Director
fused to it. She calls "CUT!" The reels burn out, color floods back, and the
credits roll. This time they open with "Stunts: Ruby," followed by our own
credits: "A game by Claude Opus and GPT-6 Astra."

Tone: warm, funny, a little haunting. Silent-film charm, not grimdark.

## Core verbs

- Run, variable jump, wall slide/kick, drop through planks (down + jump).
- **Clap** (attack): side, up, and down-pogo.
- **Record a take**: press to start, press again to stop (max length by
  upgrade). The take loops as a silver ghost double. It can be stood on,
  presses plates, hits with recorded claps, and decoys enemies.
- Unlocked by progression:
  - **Stunt Wire** (double jump)
  - **Jump Cut** (teleport to a take)
  - **Umbrella** (glide)
  - **Overcrank** (brief slow motion)
  - **Rewind** (undo the last ~2 s)

## Structure

A hub, **the Backlot** (studio streets, the commissary, the Prop Department
shop, a locked Projection Tower), connects eight sound stages. Each stage is
a film genre with its own look, music, mechanic, enemies and boss.
Wrapping a stage (beating its boss) restores one hue to the Backlot, frees
its cast (NPCs appear in the commissary), and opens the way to the next
stage. Abilities let you return to earlier stages for secrets.

| # | Stage (film) | Look | Mechanic | Boss | Reward |
| --- | --- | --- | --- | --- | --- |
| 1 | *Safety Last Street*, silent slapstick | sepia | scaffolds, clock gears, falling facades | The Clock Tower Escape | Stunt Wire |
| 2 | *Dust & Iron*, Western | amber tint | mine carts, collapsing mesas | The Iron Horse (locomotive) | Jump Cut |
| 3 | *The Crimson Tide*, pirate swashbuckler | blue night tint | ropes and rigging, rising water | The Kraken (prop tentacles) | Umbrella |
| 4 | *Jungle Serial, Chapter 12* | two-strip Technicolor (red/green) | vines, crumbling temple | Gargantua (giant ape) | Overcrank |
| 5 | *The Monster of Hollow Manor*, horror | stark B&W expressionism | darkness and lantern light, crooked sets | The Monster (lightning) | Rewind |
| 6 | *Rocket Rangers of the Moon*, sci-fi serial | three-strip Technicolor | moon gravity, ray curtains, saucers on wires | The Robot Emperor | Rewind |
| 7 | *Night City*, noir | high-contrast B&W + neon | rain, rooftops, searchlights with rifles (takes as decoys) | The Fixer | longer takes |
| 8 | *Starlight Revue*, musical | full Technicolor | set pieces on the beat, sandbags, a paper moon | The Maestro | key to the booth |
| ★ | The Projection Booth | everything | all verbs | The Everlasting Camera | the end |

Build order: the hub plus stage 1 first, to completion (an ending that works
from day one), then stages in order. At every checkpoint the game is
finishable.

## Progression, economy, health

- **Health:** 5 frames (pips shaped like film frames). Extra frame containers
  come from the shop and from secrets.
- **Scrip** (studio tokens): dropped by enemies, found in secrets.
- **Prop Department** (shop; Otto the prop master): take slot 2, take slot 3,
  longer takes, frame containers, clapper upgrade (damage), stage map rolls,
  and gags (Ruby's hat variants).
- **Lore:** 3 lost film reels per stage plus a few in the hub (about 28).
  Each plays as silent-film intertitle cards. Also studio memos (one card
  each) and trapped actors who talk in intertitles.
- **Respawn:** hazards cost one frame and return Ruby to the room's last safe
  entry. At zero frames she wakes at the last dressing-room mirror (the save
  point), keeping scrip.
- **Save:** dressing-room mirrors save to /retake/save.dat.

## Characters

- **Ruby:** the stunt double. Capable, wry, never credited.
- **Vivian Lark:** the star, trapped in the final stage. Kind and
  overwhelmed, and the only one who knew Ruby's name.
- **Augustin Vale:** the director. A genius who could not bear for anything
  to end.
- **Otto:** the prop master. Escaped into the props cage; runs the shop.
- **Stage casts:** a sheriff stuck repeating his line, a pirate chorus, the
  serial's narrator, a mad scientist, a moon cadet, a crooner, chorus girls.

## Art and audio direction

- The world is in the film stock of its stage. Ruby is always full color, and
  takes are silver.
- Readability: foreground solids are light with dark outlines; backgrounds
  are darker and lower contrast; backdrops are hazy.
- Each stage gets a musical genre:
  - 1: ragtime piano
  - 2: harmonica and guitar
  - 3: sea shanty accordion
  - 4: drums and marimba
  - 5: organ and theremin
  - 6: theremin and pulse synth
  - 7: muted jazz
  - 8: big band
  - finale: everything
- Intertitle cards (Art Deco border, white on black) carry all dialogue and
  lore.
