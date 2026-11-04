# Gravepowder asset tooling

The game's models are free CC0/CC-BY glTF files converted to a tiny custom binary
format (**GPM v1**, spec in [`../docs/assets.md`](../docs/assets.md)) and embedded
into the `.3dsx` with `bin2o`. The converted `data/models/*.gpm` files are
committed, so **CI and normal builds need no Python** ? they just run `make`.

The SD-card swap path lets you replace any model without rebuilding: the game
tries `sdmc:/3ds/Gravepowder/models/<name>.gpm` first, then the embedded copy,
then a procedural placeholder.

## One-time requirements (host, only when converting)

- Python 3.8+ on `PATH`
- `pip install pygltflib numpy Pillow`

## Convert the source models

```bat
tools\convert_assets.cmd
```

Runs `tools/import_model.py` for every `assets/import/*.json` and writes
`data/models/*.gpm`, printing per-model stats plus a size table. It validates
the GPM magic/header, prints **FAILED** and exits with code 1 if anything goes
wrong (the build then keeps the procedural fallback for that model).

Config ? embedded file name (a `"target"` field in the JSON overrides it):

| config | `.gpm` | bin2o symbol (header in `build/`) |
|---|---|---|
| `assets/import/hero_medieval.json` | `data/models/hero.gpm` | `hero_gpm` (`hero_gpm.h`) |
| `assets/import/zombie_animated.json` | `data/models/zombie.gpm` | `zombie_gpm` (`zombie_gpm.h`) |
| `assets/import/gun_flintlock.json` | `data/models/gun_flintlock.gpm` | `gun_flintlock_gpm` |
| `assets/import/gun_revolver.json` | `data/models/gun_revolver.gpm` | `gun_revolver_gpm` |
| `assets/import/gun_musket.json` | `data/models/gun_musket.gpm` | `gun_musket_gpm` |

Each generated header declares `extern const uint8_t <name>_gpm[]`,
`extern const uint8_t <name>_gpm_end[]` and `static const size_t <name>_gpm_size`.
`source/*.c` picks them up behind `#if __has_include("<name>_gpm.h")` guards so a
fresh clone (no converted models) still builds ? the renderer just uses the
procedural meshes.

## Embed them in the build

```bat
make
```

`data/models` is listed in `DATA` in the Makefile with a `%.gpm.o %_gpm.h : %.gpm`
bin2o rule, so the models land in `.rodata` and the `.3dsx` grows by the total
GPM size (~1 MB for the launch set). Nothing to change in CI: the files are
committed and `ubuntu-latest` runner + `devkitpro/devkitarm` container is enough.

## Install on the SD card (no rebuild)

```bat
tools\install_assets.cmd            :: default drive E:
tools\install_assets.cmd F:\        :: any drive / mount path
```

Copies `data/models/*.gpm` to `<SD>:\3ds\Gravepowder\models\`. The script only
copies ? it never deletes anything ? and if the drive is missing it explains
how to find the right letter and exits without touching the filesystem. The
files keep their names, exactly like the embedded ones: hero.gpm,
zombie.gpm, gun_flintlock.gpm, gun_revolver.gpm, gun_musket.gpm.

To make a model fall back again, remove its `.gpm` from the SD folder.

## Add or replace a model

1. Put the `.glb` in `assets/models/`.
2. Add `assets/import/<name>.json` with at least `"source"` and `"kind"`
   (see the existing configs; `parts`, `animations`, `resample_hz` control what
   gets exported).
3. Add the model + author + licence + source URL to `assets/LICENSES.md` (CC-BY
   models must be credited in the README/release notes too).
4. Run `tools\convert_assets.cmd`, then `make` (embed) or
   `tools\install_assets.cmd` (SD swap).
5. If the model should be one of the built-in runtime roles, make sure the
   output name matches: `hero.gpm`, `zombie.gpm`, `gun_flintlock.gpm`,
   `gun_revolver.gpm`, `gun_musket.gpm` (or use a `"target"` field).

## Troubleshooting

| symptom | fix |
|---|---|
| `python not found on PATH` | install Python 3.8+ and re-open the shell |
| `missing Python packages` | `pip install pygltflib numpy Pillow` |
| `tools\import_model.py not found` | engine-core's importer must be checked in before converting |
| converter prints `FAILED` | read the importer error above it; nothing broken in the build, fallback is used |
| game still shows the old model | the SD override wins: delete/replace the `.gpm` in `3ds\Gravepowder\models\` or re-run install with the new files |
| game shows procedural placeholder | `.gpm` missing or invalid (bad magic) ? re-run the converter/install |

## Release packaging

`release.yml` publishes a second archive, `Gravepowder-models.zip`, containing
`3ds/Gravepowder/models/*.gpm` plus `ATTRIBUTION.md` ? unzip it at the root of
the SD card for the SD-swap path. `Gravepowder.3dsx` keeps the embedded models.

## The importer (`tools/import_model.py`)

The converter script above just loops over `assets/import/*.json`; the real work
is this single-file Python tool. It can be run directly (the source model can
come from the config's `"source"` field):

```bat
python tools\import_model.py assets\models\hero_medieval.glb ^
    --config assets\import\hero_medieval.json --out data\models\hero.gpm ^
    --credit "Quaternius - Medieval pack - CC0" --preview build\previews\hero.png
```

| option | meaning |
|---|---|
| `[source.glb]` | positional; optional if the config has `"source"` |
| `--config FILE` | required import config (`assets/import/*.json`) |
| `--out FILE` | required output `.gpm`; the file is re-read and validated before the command succeeds |
| `--kind hero|zombie|weapon` | optional override/confirmation of the config `"kind"` |
| `--credit TEXT` | attribution string stored in the GPM string table |
| `--preview PNG` | orthographic 3/4 render of the result (Pillow) |
| `--preview-clip ROLE` / `--preview-time SEC` | pose the preview with a clip (default: rest pose) |
| `--preview-rest` | keep the rest pose even though the model has clips |
| `--preview-yaw/--preview-pitch DEG` | preview camera angle (`--preview-yaw 0` = look at the model's +Z side, `90` = side view with +Z to the right) |
| `--preview-wireframe` | overlay triangle wireframe on the preview (off by default) |
| `--stats`, `--bones` | print the full stats block / bone table |
| `--thin-rotation-deg`, `--thin-translation-m`, `--no-thin` | animation keyframe thinning tolerances |
| `--quiet` | only print errors (used by the batch converter) |

What the importer does, in short (details in the module docstring and
`docs/assets.md`):

- reads GLB via `pygltflib`, merges all skins by joint name, rebuilds one
  parent-before-child bone table with local rest TRS, and rigid-dominates each
  vertex onto its strongest joint;
- bakes base-colour textures (sampled with Pillow), linear `baseColorFactor` and
  `COLOR_0` into 8-bit vertex colours, with per-mesh/material
  `colour_overrides` (replace) and `colour_multiply` (composed display-space
  gains; keeps texture/material detail);
- resamples the configured animations at `resample_hz` (30 Hz for characters),
  converts quaternions to shortest-arc unit quats and thins redundant keys with
  a Ramer-Douglas-Peucker pass (bounded interpolation error, no flattened
  swings);
- normalises each model to metres (`hero` 1.80 m tall, `zombie` 1.70 m, weapon
  longest dimension 1.00 m; config `scale` multiplies that) and validates the
  written file by reading it back.

### Import config keys

| key | meaning |
|---|---|
| `kind` | `hero`, `zombie` or `weapon`; picks the metre-scale target |
| `source` | path to the `.glb` (the CLI positional overrides it) |
| `scale`, `yaw`, `offset` | extra uniform scale / Y rotation (degrees) / translation after normalisation |
| `parts` | mesh-node names to export; other meshes are dropped (bones are kept) |
| `attachments` | role -> bone-name hints (`head`, `tail`, `gun`, ...); validated and reported, never stored in the GPM |
| `animations` | role (`idle`/`walk`/`run`/`shoot`/`reload`/`attack`/`hit`/`death`) -> source clip name |
| `resample_hz` | animation sampling rate (30 for characters, 0 for static props) |
| `texture_filter` | `nearest` (default, right for palette textures) or `linear` |
| `colour_overrides` | mesh-node or material name -> `[r,g,b(,a)]` display colour that **replaces** the baked colour |
| `colour_multiply` | mesh-node or material name -> `[r,g,b(,a)]` **display-space gain** applied on top of the baked colour; part, material and `"*"` entries compose (multiply), values > 1 brighten. The hero uses this to lift its very dark source materials into dust-lit leather while keeping detail |
| `target_height` / `target_length` | override the canonical metre target for the kind |
| `credit` | attribution string written into the GPM header |

**Coordinate convention (the runtime must match):** GPM space is right-handed,
**+Y up, +Z forward**, metres. Characters are grounded at y=0 and centred on
x/z; **characters face +Z** (the renderer's `Mtx_RotateY(...yaw)` assumes the
model's forward is +Z). Static guns are centered on their bbox with the
**muzzle pointing at +Z** (renderer viewmodel/hand mount use local +Z as the
barrel direction). Skinning is `anim_world[bone] * inverse(rest_world[bone]) *
position` with rest/anim bone matrices built from the stored local TRS.

## Validating / inspecting a GPM (`tools/gpm_selftest.py`)

The importer validates every file it writes, but you can re-check committed
files (or a model from the SD card) at any time:

```bat
python tools\gpm_selftest.py data\models\*.gpm --bones --clips
python tools\gpm_selftest.py data\models\gun_musket.gpm --preview build\musket.png
```

It re-parses the file, checks every offset/count/string/quaternion/index bound,
prints a stats table (vertices, triangles, bones, clips, bbox, size, estimated
skinning cost), and exits non-zero on any structural error. `--bones` prints the
exact joint names the renderer resolves attachments against (`Head`, `Hips`,
`Wrist.R` for the hero). `--json` emits the same stats machine-readably.
