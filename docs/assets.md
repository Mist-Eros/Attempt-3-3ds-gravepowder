# Asset pipeline (modular models)

The goal: **you can swap the hero, zombies or guns without touching any renderer code** — drop a
converted `.gpm` file on the SD card and the game picks it up. The repo ships built-in defaults, so a
missing or broken asset file just falls back.

## TL;DR for swapping a model

1. Put a `.glb` (glTF binary) model in this folder.
2. `pip install pygltflib numpy Pillow` once, then `python tools/import_model.py assets/models/mymodel.glb --kind hero --config assets/import/hero.json --out build/hero.gpm`
3. Copy `build/hero.gpm` to `sdmc:/3ds/Gravepowder/models/hero.gpm` on the SD card.
4. Boot the game — it loads the SD model if present, otherwise the embedded one.

The importer prints vertex/triangle/bone/animation counts and the runtime cost estimate. Later this
will also be exposed as an in-game "mods" screen (load from SD, show author/licence from the header).

## The GPM v1 format ("Gravepowder Model")

Little-endian binary. Everything is one file. `tools/import_model.py` is the reference writer;
`source/gpm.c` is the reference reader. Keep the two in sync if the format ever changes.

### Header — 64 bytes

| offset | type | field | notes |
|---|---|---|---|
| 0 | u32 | magic | `0x31504D47` = `"GPM1"` |
| 4 | u32 | version | 1 |
| 8 | u32 | flags | bit0 skinned, bit1 has animations |
| 12 | u32 | vertex_count | |
| 16 | u32 | index_count | multiple of 3 |
| 20 | u32 | bone_count | 0 for static models |
| 24 | u32 | anim_count | 0 if no animation |
| 28 | f32[3] | bbox_min | local space, metres |
| 40 | f32[3] | bbox_max | |
| 52 | u32 | off_strings | NUL-separated names (bones, animations, credit) |
| 56 | u32 | off_sections | offset of the section-offset table below |
| 60 | u32 | file_size | sanity check |

At `off_sections`: `u32 off_vertices, off_indices, off_bones, off_anims, off_tracks, off_keys` (24
bytes), each an absolute byte offset from the start of the file.

### Vertex — 32 bytes

| type | field | notes |
|---|---|---|
| f32[3] | position | local space, rest pose |
| f32[3] | normal | rest pose, normalised |
| u8[4] | colour | RGBA, baked from the source texture/material |
| u16 | bone | dominant bone index (0 for static models) |
| u16 | pad | zero |

We use **rigid skinning**: every vertex follows exactly its dominant bone. The importer welds weights
to the strongest joint and re-normalises on export. For the low-poly style this is fine (PS1-era
skinning) and *much* cheaper on the 3DS CPU than 4-influence blending.

### Indices — `index_count × u16`

### Bone — 48 bytes

| type | field | notes |
|---|---|---|
| i16 | parent | -1 = root |
| u16 | pad | |
| f32[3] | translation | local rest translation |
| f32[4] | rotation | local rest rotation, quaternion (x,y,z,w) |
| f32[3] | scale | local rest scale |
| u32 | name_off | offset into strings |

Bones are stored in parent-before-child order. Rest pose = local TRS. The loader precomputes
`world_rest = parent_world * T*R*S` and `inverse_bind = inverse(world_rest)`.

### Animation — 16 bytes

| type | field | notes |
|---|---|---|
| u32 | name_off | one of the standard clip names (see below) |
| f32 | duration | seconds |
| u32 | track_first | index into tracks |
| u32 | track_count | |

### Track — 8 bytes

| type | field | notes |
|---|---|---|
| u16 | bone | |
| u8 | component | 0 = translation, 1 = rotation (quant), 2 = scale |
| u8 | pad | |
| u16 | key_count | |
| u16 | pad | |

### Key — 20 bytes

| type | field | notes |
|---|---|---|
| f32 | time | seconds |
| f32[4] | value | rotation = quat; translation/scale = xyz + pad 0 |

Keys are sorted by time. Sampling: linear interpolation for translation/scale, shortest-arc slerp for
rotation. If the clip has no key for a bone/component, use the rest pose value.

### Standard animation clip names

The gameplay asks for clips by *role*, not by source name; the importer maps them via the config
file. Recognised role names:

`idle`, `walk`, `run`, `shoot`, `reload`, `attack`, `hit`, `death`

An imported model may provide any subset — the renderer falls back to the nearest available clip
(`shoot`→`idle`, `run`→`walk`, `hit`→`idle`, …) and finally to the rest pose.

## Import configs (`assets/import/*.json`)

Small JSON files the importer reads, e.g.:

```json
{
  "kind": "hero",
  "scale": 1.0,
  "yaw": 180.0,
  "parts": ["Medieval_Body", "Medieval_Legs", "Medieval_Feet"],
  "attachments": {
    "head": "Head",
    "tail": "Hips",
    "gun": "RightHand"
  },
  "animations": {
    "idle": "CharacterArmature|Idle_Gun",
    "walk": "CharacterArmature|Walk",
    "run": "CharacterArmature|Run",
    "shoot": "CharacterArmature|Gun_Shoot",
    "hit": "CharacterArmature|HitRecieve",
    "death": "CharacterArmature|Death"
  },
  "colour_overrides": {
    "Medieval_Body": [0.55, 0.31, 0.18, 1.0]
  },
  "resample_hz": 30
}
```

`parts` selects which mesh nodes to export (useful for modular packs — we drop the imported head and
use the procedural wolf head instead). `attachments` names bones the renderer can hang procedural
parts off (wolf head/ears/tail, the gun in the hero's hand). `resample_hz`: importer resamples source
animations to this rate and bakes textures into vertex colours.

## Runtime loading

The game looks for these files, in order:

1. `sdmc:/3ds/Gravepowder/models/<name>.gpm` (user override, no rebuild needed)
2. embedded copy in the `.3dsx` (built from `assets/models/*.glb` at build time)

Names: `hero.gpm`, `zombie.gpm` (one zombie model reused for shambler/runner/brute with per-type
scale/tint), `gun_flintlock.gpm`, `gun_revolver.gpm`, `gun_musket.gpm`. Missing or invalid files fall
back to the procedural built-in meshes.

The `attachments` map in the import config is **not stored in the GPM file** (the format is frozen):
the importer validates the names against the exported skeleton, and the runtime resolves them by
scanning the bone name table (e.g. `Head`, `Hips`, `Wrist.R`).

## Performance budget on the 3DS

Software skinning is CPU work; keep it lean.

- Hero: ≤ ~9,000 skinned vertices (body+legs+feet after dropping the head) — ~9k matrix transforms/frame.
- Zombies: ≤ ~4,500 vertices each. The renderer should *LOD* them: full skinning only for the nearest
  N zombies; distant ones can reuse a cached pose or draw an unskinned rest pose (the importer can
  optionally export a static `*.pose.gpm` variant).
- Guns/props are static: cheap.
- Animations are resampled at 30 Hz, so draw-rate and sample-rate stay decoupled.

Target: keep the total skinned vertex count per frame under ~20,000 at 60 fps. If it goes over, lower
the zombie LOD distance or ship `zombie_*.pose.gpm` for the far ones.

## Why this format

- one file per model, no runtime glTF parser on the 3DS (glTF parsing in C would be far heavier than
  this whole format)
- dominant-bone rigid skinning keeps the CPU cost predictable
- vertex colours (baked from textures) keep the flat-shaded look and avoid a texture pipeline on the
  3DS for now
- every model can be replaced from the SD card, and the importer is a single Python script you can
  run at home

## Future

- weight blending (2 influences) if rigid skinning shows cracks on the user's model
- textures via tex3ds once the art direction calls for it
- a Blender exporter mode (the importer already understands glTF, so "export glTF" is enough)
