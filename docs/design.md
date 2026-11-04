# Gravepowder — design notes

A 3D zombie shooter for the Nintendo 3DS. Anthropomorphic hero, flintlock-era guns, shambling
undead, and a Borderlands-ish "clear the zone, farm the drop" structure — but small enough to
actually finish as homebrew.

This file is the running design + implementation spec. Numbers live in the source tables
(`source/core/data.c`, `include/grave.h`); this doc explains *why*.

## Pitch

You are a wolf-folk gunslinger (working name: **Rook**) who wandered into a valley that the dead
have overrun. Old guns only: a flintlock pistol, a cap-and-ball revolver, a long musket. The valley
is split into zones; each zone has a job — clear out the rot, or bring back a material the survivors
need. Materials drop from zombies. Clear the job, get paid, move on.

Tone: dusty, grim, a little silly. Borderlands by way of a Western penny dreadful.

## v0.1 scope (first playable)

One zone, playable start to finish, in both camera modes:

- **Zone: ASHEN MILL** — a walled yard: broken fence, crates, barrels, a well, dead trees, and a
  gate. Ground is dirt. One way in, one way out.
- **Hero:** original low-poly anthro wolf (see Art direction), third-person by default; **first
  person toggle** in the pause/settings menu and with a face button shortcut.
- **Weapons:** FLINTLOCK (one shot, hits hard, slow reload), REVOLVER (six shots, faster),
  MUSKET (one shot, brutal, very slow, long range). Hitscan with muzzle flash, smoke, tracer,
  impact sparks. Reload timers, ammo, weapon switching.
- **Zombies:** SHAMBLER (slow, common), RUNNER (fast, fragile), BRUTE (slow, tanky). They shuffle
  toward you, wind up, and swipe. Head/body damage is not modeled yet — flat damage.
- **Quest:** clear the zone — kill N zombies; a portion of them drop OLD COINS and ROT SINEW that
  count toward a collect step. HUD quest tracker, materials counter, quest complete screen.
- **HUD (bottom screen):** health bar, ammo/reload readout, three weapon slots, quest tracker,
  materials, a small radar with zombie blips, pause button. Title/pause/quest-complete/game-over
  screens in the same old-paper style language (see UI direction).
- **Audio:** synthesized musket/revolver/flintlock reports, reload clacks, zombie groans, hit
  thuds, wind ambience and a sparse music loop. No ripped audio.
- **Persistence:** none in v0.1 (zone restart is cheap).

Later milestones:

- v0.2 — second zone (BONE ORCHARD) + hub "trail" screen between zones, runner/brute variety,
  pickups on the ground, save file on the SD card, first/third person polish, more props.
- v0.3 — third zone (OLD MINE) with a boss-ish brute, quests that send you between zones, shop
  for ammo/upgrades, boss bar, ending screen.
- v1.0 — polish pass, stereo 3D option, real hardware shakeout.

## Controls

| Input | Third person | First person |
|---|---|---|
| Circle pad | move (camera-relative) | move (camera-relative) |
| Touchscreen drag | orbit camera / aim | look / aim |
| A | fire | fire |
| B | reload | reload |
| X | switch weapon | switch weapon |
| Y | toggle first/third person | toggle first/third person |
| L/R | sprint / strafe (R held) | sprint / strafe |
| Start | pause | pause |

Touchscreen drag is the aim input on hardware (the 3DS has no second stick on the base model); a
touch drag of N pixels maps to a yaw/pitch delta. A tap on the bottom screen's pause button works
too. On Azahar the keyboard maps to the same inputs.

## Art direction

**Everything is original, generated in code — no ripped models or textures.**

- Low-poly, flat-shaded geometry (citro3d, vertex colours or fixed-attribute shading per face),
  240p-friendly silhouettes. Think DS-era 3D with a modern palette.
- **Hero:** anthro wolf made from simple primitives/segments (head with muzzle + ears, torso, tail,
  arms, legs), palette: dusty russet fur, cream muzzle/chest, dark leather coat, brass details.
  Procedural walk cycle (limb swing), idle breath, a toothy grin on the title screen.
  The hero rig is a **separate module with a clean mesh format** so the user's own model can replace
  it later (a simple indexed vertex format + optional animation segments; see `source/mesh.h`).
- **Zombies:** same segmented-body approach, hunched, arms out, tattered browns/greens, glowing pale
  eyes (unlit quads), head lolls. Death = fall over + sink/fade.
- **Guns:** chunky low-poly flintlock/blunderbuss shapes with brass + dark wood, muzzle flash quad,
  smoke puff.
- **Zone:** dirt ground with vertex-colour mottling, broken fence posts, tombstone-ish stones,
  crates/barrels, dead trees, a well, gate frame, muted browns/greys with sickly green accents.
- **UI:** old-paper / weathered sign look; serif-ish chunky lettering via citro2d text; brass and
  bone colours, red accents; hand-drawn-ish borders. No modern flat UI.

## Technical architecture

Three layers, the same pattern that worked for the tower defense:

```
include/grave.h      portable game contract (types, tables, events, input)
include/render3d.h   3D front-end contract (targets, scene begin/end, draw world)
include/app.h        app/UI contract (init, step, draw bottom HUD)
include/audio.h      audio contract (events + music tracks)

source/core/         portable C99 simulation (no <3ds.h>): player, weapons, zombies,
                     pickups, quests, spawner, collision, camera
source/render3d.c    citro3d: meshes, animation, camera, lights, particles, zone draw
source/mesh.c/h      procedural mesh builders + simple animation helpers
source/ui.c          citro2d bottom screen: HUD, menus, radar
source/app.c/h       scene state machine + input mapping + audio event dispatch
source/audio.c       portable fallback (records events, no sound)
source/audio_3ds.c   ndsp mixer for the 3DS build
source/main.c        aptMainLoop/frame timing, calls r3d + app
tests/               host unit tests for the core
tools/sim.c          headless gameplay sim (AI walks a zone, shoots zombies; balance checks)
tools/preview/       2D preview harness only where useful (HUD layout)
```

Rules:

- The core is deterministic from a seed and fixed 1/60 steps. No malloc in the loop; fixed pools
  sized in `grave.h`.
- The renderer never mutates game state; it only reads `GraveGame` + camera outputs.
- The HUD never mutates game state; it reads it and writes only input flags via the app layer.
- Any gameplay number (damage, speed, drops, quest counts) lives in `source/core/data.c`.
- Build deps: devkitPro 3ds-dev only (libctru/citro3d/citro2d + picasso shaders). Audio uses
  synthesized PCM (generated by `tools/make_audio.py` and linked via bin2o) so the build stays
  self-contained.

## Milestones / done criteria

- **M1 skeleton** (done): citro3d + citro2d both screens, shader pipeline, builds locally and via CI.
- **M2 core sim**: player movement + collision, weapons, zombie AI, quest, host tests + sim green.
- **M3 render pass**: zone + hero + zombies + guns visible, animation, camera both modes; verified
  in Azahar (screenshots).
- **M4 UI pass**: HUD, menus, minimap; playable loop from title to quest complete.
- **M5 audio + polish**: sound, damage feedback, screenshake-ish kick, zone transitions.
- Then release v0.1.0 (CI-built .3dsx), install to the user's SD card.

## Known risks

- 3DS GPU draw call budget: aim for < ~400 draw calls/frame. Meshes are static VBOs in linear
  memory; per-object transforms are cheap. Instancing/zombie LOD if needed.
- Text rendering with citro2d system font is expensive-ish; cache text buffers.
- Touch aiming needs tuning; offer sensitivity + invert options early.
- The user will supply a hero model later — keep the hero mesh swappable.
