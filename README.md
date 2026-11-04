# Attempt 3: Gravepowder for the 3DS

A 3D zombie shooter for the Nintendo 3DS, built from scratch in C. Anthro wolf gunslinger, flintlock-era
iron, shambling rot, and a valley split into zones you clear one dusty yard at a time.

This is the follow-up to [Elemental Defense](https://github.com/Mist-Eros/Attempt-2-3ds-game) (the
maze-building tower defense I ported to the 3DS last time). Same approach, harder problem: real 3D on
a 240p handheld, with **free CC0/CC-BY models** (skinned and animated) so it doesn't look like
programmer art anymore.

![Gravepowder title screen](docs/screenshots/title.png)

## What the game is

You're a wolf-folk gunslinger (working name: Rook) who walked into a valley the dead have overrun.
Old guns only:

- **Flintlock pistol** — one shot, hits like a mule, slow to reload.
- **Cap-and-ball revolver** — six shots, the practical choice.
- **Long musket** — one shot, brutal damage, long range, takes forever to reload.

The valley is split into **zones**, Borderlands-style. Each zone has a job: clear out the rot, or
bring back a material the survivors need. Zombies drop old coins, rot sinew and (in the orchard) bone
charms — those are what the quests ask for. Clear the job, unlock the next zone, walk on.

**v0.2 — the valley opens up:**

- **ASHEN MILL** — the walled mill yard from v0.1. Kill 40 of the rot, bring back 8 old coins and 4
  rot sinew.
- **BONE ORCHARD** — a dense graveyard (143 props), runner-heavy waves with a brute every wave. Kill
  55, bring back 10 old coins and 6 bone charms.
- **OLD MINE** — unlocks after the orchard, stub zone for now (same layout, higher kill goal).
- **World map travel**: clear a zone, then travel between zones from the WORLD page. Progress,
  materials, owned guns and settings are saved to the SD card (`save.bin`), so the valley remembers.

Two camera modes: **third person** (default — you see the wolf, coat, tail and the gun at his hip) and
**first person**, toggled with Y or in settings.

### Screens

| Title | Third person | First person |
|---|---|---|
| ![title](docs/screenshots/title.png) | ![third person](docs/screenshots/gameplay.png) | ![first person](docs/screenshots/firstperson.png) |

| Firing | Area map | World map |
|---|---|---|
| ![fire](docs/screenshots/fire.png) | ![area map](docs/screenshots/area.png) | ![world map](docs/screenshots/world.png) |

| Inventory | Pause |
|---|---|
| ![inventory](docs/screenshots/pack.png) | ![pause](docs/screenshots/pause.png) |

## The bottom screen is a paged info screen

Top screen is the 3D gameplay. Bottom screen has a slim always-on strip (health/stamina, weapon/ammo,
pause) plus three tabs — touch them, or press **Select** to cycle:

- **AREA** — top-down map of the current zone: props, walls, gate, your facing arrow, live zombie
  blips, loot dots, objective marker, quest progress.
- **WORLD** — the valley: zone nodes with locked/cleared/current state, best times, materials summary;
  tap an unlocked zone to travel there.
- **PACK** — inventory: weapon cards with stats and ammo (tap to equip), materials, character sheet.

## Playing it

Grab `Gravepowder.3dsx` (and optionally `Gravepowder-models.zip`) from the
[releases page](https://github.com/Mist-Eros/Attempt-3-3ds-gravepowder/releases), drop the `.3dsx`
in `/3ds/` on your SD card, launch it from the Homebrew Launcher. Needs a 3DS with homebrew
(Luma3DS/CFW or any entry point that can run `.3dsx` files).

### Controls

| Input | Action |
|---|---|
| Circle pad | move |
| Touchscreen drag | aim / look |
| A | fire |
| B | reload |
| X | switch weapon |
| Y | toggle first / third person |
| L | sprint |
| Select | cycle bottom-screen pages |
| Touch tabs | switch pages / tap nodes, weapon cards, buttons |
| Start | pause / resume |

Playing under an emulator (Azahar)? Arrow keys = circle pad, WASD = d-pad, **U/H/J/K** = X/Y/B/A,
**Q/I** = L/R, **M/N** = Start/Select, and the mouse on the bottom screen is the touchscreen.

### Sound

Music and the gun/zombie sounds are original, synthesized from scratch by `tools/make_audio.py` and
linked into the `.3dsx` (raw PCM, no extra files needed). The 3DS audio service needs **DSP1
firmware** (Luma3DS); if it's missing the game runs silently instead of crashing.

## Models and licences

The hero body, zombies and guns are free models (see [`assets/LICENSES.md`](assets/LICENSES.md)):

| What | Model | Licence |
|---|---|---|
| Hero body (24 animations) | Quaternius *Medieval* | CC0 |
| Wolf head / ears / tail | procedural, attached to the imported skeleton | original |
| Animated zombie (13 animations) | *Animated Zombie* by bachosoftdesign | CC-BY 3.0 |
| Flintlock pistol / musket | Quaternius Pistol / Rifle | CC0 |
| Revolver | *Revolver* by zeilmannnoah | CC-BY 3.0 |

Models are converted from glTF to a small fixed binary format (`GPM`) by `tools/import_model.py` and
embedded in the `.3dsx`. No ripped assets are used or redistributed.

## Swapping models

Converted models live in `data/models/` and are embedded, but a copy on the SD card overrides the
embedded model at runtime — you can swap the hero/zombie/guns **without rebuilding the game**:

```
E:\3ds\Gravepowder\models\hero.gpm
E:\3ds\Gravepowder\models\zombie.gpm
E:\3ds\Gravepowder\models\gun_flintlock.gpm
E:\3ds\Gravepowder\models\gun_revolver.gpm
E:\3ds\Gravepowder\models\gun_musket.gpm
```

Delete a file to fall back to the embedded model. `Gravepowder-models.zip` in the release contains
the whole folder ready to unzip at the SD root.

To convert a model of your own:

```sh
pip install pygltflib numpy Pillow
python tools/import_model.py assets/models/mymodel.glb --config assets/import/hero.json --out data/models/hero.gpm
tools\convert_assets.cmd      # or just this: converts every assets/import/*.json in one go
make                          # re-embed (only needed for the built-in copy)
```

Config JSONs (`assets/import/*.json`) say which mesh parts to export, which source animation maps to
which gameplay role (`idle`/`walk`/`run`/`shoot|attack`/`hit`/`death`), scale/orientation and colour
tints. Full format + workflow in [`docs/assets.md`](docs/assets.md) and
[`tools/README.md`](tools/README.md). The user's own wolf model will be slotted in the same way.

## Status

v0.2 is playable end-to-end: two real zones + a stub third, zone travel, save file, animated models
both camera modes, paged touch screen with maps and inventory, original music/SFX.

Still to do:

- [ ] real-hardware playtest of v0.2 (3D, touch aim, model animation, audio)
- [ ] OLD MINE as a proper zone with a boss-ish brute, then an ending
- [ ] shop / gun upgrades between zones; accounter for shots fired (accuracy stat)
- [ ] more model swapping: the user's own anthro model, weapon variants
- [ ] performance polish: zombie LOD tuning, draw-call budget in the orchard

## Building it

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS dev packages:

```sh
pacman -S 3ds-dev
make
```

That produces `Gravepowder.3dsx`, `Gravepowder.elf` and the `.smdh`. The converted models are
committed, so a normal build needs no Python; only re-converting models does (see above). Host tests
and the headless campaign sim:

```sh
make -C tests CC=cc test     # 3265 checks
make -C tools CC=cc run     # bot clears Ashen Mill + Bone Orchard
```

Every push and every `v*` tag builds it in GitHub Actions.

## How it's put together

```
include/grave.h      portable game contract (world, quests, inventory, save, input)
include/gpm.h        model format loader API
include/skin.h       CPU skinning + animation sampling API
include/render3d.h   3D front-end contract (targets, scene, draw world, model swap)
include/app.h        app/UI contract
include/audio.h      audio contract

source/core/         portable C99 simulation (no <3ds.h>): player, guns, zombies, drops,
                     world/travel, quests, camera, save image
source/gpm.c         GPM model loader (embedded or SD override)
source/skin.c        rigid-bone CPU skinning, clip sampling, crossfade, LOD
source/mesh.c        procedural fallback meshes + wolf head/tail attachments, props, effects
source/render3d.c    citro3d world rendering, camera, lighting, particles
source/ui.c          citro2d paged bottom screen (strip + AREA/WORLD/PACK)
source/app.c         scene state machine, input, save I/O, audio dispatch
source/main.c        aptMainLoop frame loop (fixed 60 Hz)
source/audio*.c      portable fallback + libctru ndsp mixer
assets/              free CC0/CC-BY model sources + conversion configs + licences
tools/               importer, selftest, asset convert/install scripts, audio synth, sim
tests/               host unit tests
```

The core is plain C99, no malloc in the loop, deterministic from a seed. The renderer only reads core
state; gameplay numbers live in `source/core/data.c`; models are data, not code.

## Credits

- [devkitPro](https://devkitpro.org/), libctru, citro3d and citro2d.
- Model assets (CC0/CC-BY) as listed above and in [`assets/LICENSES.md`](assets/LICENSES.md):
  Quaternius (hero body, pistol, rifle), bachosoftdesign (animated zombie, CC-BY 3.0), zeilmannnoah (*Revolver*, CC-BY 3.0).
- Everything else — the wolf head/ears/tail, props, effects, UI art, sounds and music — generated by
  the code in this repo.
- The wolf, the valley and the very bad idea of a flintlock-only zombie apocalypse are mine.
