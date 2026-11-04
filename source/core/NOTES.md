# Portable gameplay core — implementation notes (v0.2, "Ashen Mill" + "Bone Orchard")

Everything in `source/core/` is plain C99, no `<3ds.h>`, no `malloc` in the loop,
fixed pools from `include/grave.h`. The renderer / UI / audio layers only **read**
`GraveGame` (plus the camera product); the app drives input, owns the buttons and
owns the SD card file I/O.

```
source/core/game.c        the simulation (implements include/grave.h)
source/core/data.c        ALL gameplay numbers: weapons, zombie stats, zone tables
source/core/core_internal.h  internal-only tuning + ZombieSpec/ZoneSpec (NOT public)
source/core/input.c       input helpers (unchanged)
tests/test_core.c         host unit tests: 3265 checks, deterministic, no 3DS headers
tools/sim.c               headless campaign sim (Ashen Mill -> Bone Orchard) + balance
```

v0.2 added **only** the following to `include/grave.h` (all additive, nothing
renamed/removed):

- enum values: `ZONE_OLD_MINE` (before `ZONE_COUNT`), `DROP_BONE_CHARM` (before
  `DROP_COUNT`)
- new types: `World`, `ZoneNode`, `SaveData`, `GraveSettings`
- appended struct fields: `Player.weapon_owned[]`;
  `GraveGame.world, material_bone_charms, run_materials[], total_kills,
  total_time, zone_runs, spawn_wave_left, spawn_wave_brutes`
- API: `grave_world_init/unlock/unlocked/cleared/to_trail`, `grave_game_travel_to`,
  `grave_inventory_owned/weapon_count/ammo/material`, `grave_save_size`,
  `grave_save_to_buffer`, `grave_load_from_buffer`, and the `GRAVE_SAVE_*`
  macros.

---

## Ownership split (app layer contract)

The core consumes from `GraveInput`: `move_x`, `move_y`, `look_x`, `look_y`,
`fire` (held), `sprint` (held), `reload` (tap), `swap_weapon` (tap),
`toggle_camera` (tap).

Weapon switching and camera mode are supported **both** ways, because the app
does both: in-game X/Y arrive as edge taps (`input.swap_weapon` /
`input.toggle_camera`) and the core applies them; the pause/settings menus write
`game->player.weapon` / `game->player.camera_first_person` directly and the core
honours those values. An app-side weapon change during a reload cancels the
reload (`Player.reload_weapon` bookkeeping) — no free rounds. A zone start keeps
the current weapon, camera preference and owned-gun flags.

`pause` is app-owned: the app freezes stepping; `game->phase` may stay `PLAYING`
while paused. If the app does set `GAME_PHASE_PAUSED`, the core no-ops until it
is set back. `input.pause` / `input.interact` are still unused by the core.

`game->phase` is authoritative: `GAME_PHASE_QUEST_COMPLETE` when the quest is
done, `GAME_PHASE_GAME_OVER` on death. Stepping does nothing in TITLE / PAUSED /
QUEST_COMPLETE / GAME_OVER.

`dt` is expected to be a fixed `1/60`; the core clamps anything outside
`(0, 0.25]` and can be called 0..N times per frame.

## Events (`game->events`)

`grave_game_step` **ORs** `GRAVE_EVT_*` bits into `game->events`; it never clears
the word. The app does `ev = game->events; game->events = 0;` after every step,
then dispatches. A bit is raised exactly once per occurrence, never on idle
frames.

- `SHOOT` per shot; `RELOAD` when a reload **starts**; `EMPTY_GUN` on a dry
  trigger; `HIT` per shot that hits a zombie; `ZOMBIE_DIE` on the killing hit;
  `PICKUP` per pickup collected; `HURT` per zombie swipe that lands;
  `QUEST_DONE | ZONE_DONE` together on completion.
- No footstep/death bits: derive footsteps from `Player.anim_speed`; watch
  `phase` for game over / results.

## Camera product

`grave_update_camera(game, aspect)` fills `camera_pos`, `camera_look`,
`camera_fov`; it ignores `aspect` (the renderer owns the projection). The step
function never reads these.

- Metres + radians; `Mtx_LookAt(camera_pos, camera_look, (0,1,0))`.
- `camera_fov` is the **vertical FOV in degrees** (62 third person, 68 first
  person) → `C3D_AngleFromDegrees`.
- `camera_look - camera_pos` is exactly the aim direction (crosshair = shot
  line). Third person orbits at 3.2 m behind a pivot 1.55 m up, 0.45 m over the
  shoulder, and pulls in instead of clipping props/walls/floor.
- `player.recoil` is already folded into the camera pitch; don't add it again.

## Hero

- `grave_hero_root`: feet position (y = 0) + facing yaw (yaw 0 = +z, positive
  turns right, towards +x).
- `grave_hero_walk_phase`: `player.anim_time` in radians for `sin()`-driven limb
  swing; `player.anim_speed` is 0 idle / 1 walk / ~1.35 sprint.
- `player.firing` / `muzzle_flash` (counts down from 0.07 s) drive the flash.

## Zombies

- `state`: `IDLE` (spawn notice) → `CHASE` → `ATTACK` (windup then swipe, then
  cooldown; stays in ATTACK while the player is in reach) or `STAGGER` (hit
  reaction) → `DYING` (`death_time` 0..1 over 1.15 s) → `DEAD` (`active=false`).
- `anim_time` runs continuously, `speed` is current m/s, `yaw` faces the player.
- `attack_timer`: in ATTACK, seconds until the next swipe lands / next swing.
- Hit volumes: body box (`radius` × `body_height-0.26`) + head sphere at
  `body_height-0.13` (r 0.13); "head-ish" hits do 1.5× damage. Head shots are not
  announced separately — a tracer ending near head height is the tell.
- `zombies_alive` includes dying bodies until the fade ends; radar can skip
  `DYING`/`DEAD`.
- Spawns are rejection-sampled against props/each other and are ≥7 m from the
  player, so nothing starts inside geometry.

### Spawner (v0.2 pending-wave model)

Waves are queued **as a whole** (3 / 4 / 5 members as the budget is consumed) and
released as the `max_alive` cap frees slots. `ZoneSpec.brute_per_wave` therefore
means literally one brute per wave even when only one slot opens at a time. The
spawn timer only starts the next wave once the previous one is fully released.

## Pickups

- `Pickup.pos.y` is a base height (0.35 m); add the bob from `bob_time`; `life`
  counts down from 60 s; auto-collected within 1.55 m of the player.
- Materials (`DROP_COIN`, `DROP_SINEW`, `DROP_BONE_CHARM`) increment the
  inventory totals **and** `run_materials[kind]` for the current zone run.
  `DROP_AMMO` tops the current weapon up by 2 rounds (clamped).
- Zone-generic drop cycle per kill: `[material A, material B, material A,
  (powder 60% | material A)]`, so `kill_goal` kills always drop far more of both
  quest materials than the collect steps need (no soft-lock).

## Props

- `Prop.pos` is the centre of the base; box is `[pos±half] × [pos.y ..
  pos.y+2*half.y] × [pos.z±half.z]`. Collision treats props as full-height XZ
  footprints and ignores `yaw`; rendering may rotate them.
- `color` = `(uint8_t)kind` (0..7 crate/barrel/fence/tombstone/deadtree/well/
  gate).
- Layouts are hand-authored and deterministic (no RNG): ASHEN MILL = 90 props
  (yard + well + crates + barrels + a few tombstones/trees); BONE ORCHARD = 143
  props (dense gravestone rows + dead orchard trees + a little entrance cover);
  OLD MINE is a v0.2 stub that reuses the mill layout for now.
- Player bounds ±19.2 m; player spawns at `(0, 0, 15.5)` facing -z.

## Quests (generic per zone)

Each zone runs the same ladder, driven by its `ZoneSpec`:

1. `QUEST_CLEAR_ZONE`: `target = spawn_kill_goal`, `progress = zombies_killed`.
2. `QUEST_COLLECT`: `material = material_a`, `target = goal_a`,
   `progress = run_materials[material_a]`.
3. `QUEST_COLLECT`: `material = material_b`, `target = goal_b`,
   `progress = run_materials[material_b]`.
4. `QUEST_DONE` + `GAME_PHASE_QUEST_COMPLETE` + `QUEST_DONE|ZONE_DONE` events.
   Stages can hop several at once if the player already has the goods; the HUD
   should render `quest.kind/target/progress/material` generically
   (`grave_drop_name(quest.material)` is the label).

| zone | kill goal | collect A | collect B | alive cap | wave |
|---|---|---|---|---|---|
| ASHEN MILL | 40 | 8 old coins | 4 rot sinew | 8 | 2.8 s |
| BONE ORCHARD | 55 | 10 old coins | 6 bone charms | 8 | 3.0 s, 1 brute/wave, runner-heavy |
| OLD MINE (stub) | 50 | 12 old coins | 6 rot sinew | 8 | 3.0 s |

## World / travel progression (v0.2)

`GraveGame.world` is the WORLD-map model:

- `nodes[i]` = `{ zone, unlocked, cleared, clears, best_time }` for every
  `ZoneId` (ASHEN MILL, BONE ORCHARD, OLD MINE).
- `world.current` = zone being played (or the map cursor while on the trail);
  `world.in_zone` = false on the trail/hub.

API:

- `grave_game_init` unlocks ASHEN MILL only and puts the player on the trail.
- `grave_game_start_zone(game, zone)` now **respects unlocks**: if the node is
  locked it does nothing at all (the game state is untouched). Otherwise it
  resets the run (pools/quest/run_materials/spawner/timers/RNG) and sets
  `world.current` + `in_zone = true`. Inventory (materials, owned guns, ammo,
  camera preference), lifetime stats and world node flags persist.
- `grave_game_travel_to(game, zone)` = unlock check + start; returns bool for the
  WORLD page's "travel" button.
- `grave_world_to_trail(game)` leaves the zone and sets `phase = TITLE` (the app
  switches to its WORLD/AREA/PACK pages; the core just stops stepping).
- Completing a zone marks its node `cleared`, records `best_time`/`clears` and
  **unlocks the next node in the chain**. `grave_world_unlock` is there for
  scripted/debug unlocks.
- UIs can read everything directly from `game->world`; no extra query calls
  needed beyond the helpers above.

## Inventory (PACK page)

- Owned guns: `player.weapon_owned[WEAPON_COUNT]` (all true in v0.2; save-backed
  for later milestones).
- Ammo: `player.ammo[WEAPON_COUNT]` = loaded rounds per gun (no reserve pool in
  v0.2; reloads refill from the ether, the cost is reload time).
- Materials: `material_coins`, `material_sinew`, `material_bone_charms`
  (inventory totals; persist across zones and saves).
- Read-only helpers for the UI:
  `grave_inventory_owned(game, id)`, `grave_inventory_weapon_count(game)`,
  `grave_inventory_ammo(game, id)`, `grave_inventory_material(game, drop)`.
  Weapon stats come from `grave_weapon_spec(id)` (name/damage/range/magazine/
  reload/fire interval/recoil) — that is everything the PACK page needs.
- `run_materials[DROP_COUNT]` is the *per-zone-run* tally the quest stages read;
  reset on every zone start. HUD shows the quest with `quest.*`, the roster with
  the inventory totals.

## Save data (v0.2)

Portable, fixed-size, versioned, checksummed image — **the core does not touch
the SD card**; the app/main does file I/O. This keeps the core host-testable and
lets main own the platform layer.

```c
#include "grave.h"

uint8_t image[GRAVE_SAVE_SIZE];      /* GRAVE_SAVE_SIZE == 92 */
int n = grave_save_to_buffer(&game, &app_settings, image, sizeof image);
/* write exactly n bytes to sdmc:/3ds/Gravepowder/save.bin (create the folder) */

/* boot: read up to sizeof image bytes, then */
if (!grave_load_from_buffer(&game, &app_settings, image, read_bytes)) {
    /* no/corrupt save: keep the fresh grave_game_init() state */
}
```

Recommended 3DS flow (main/app layer, not core):
1. `mkdir("sdmc:/3ds/Gravepowder", 0777)` (ignore EEXIST).
2. Save: serialise to the buffer, write `sdmc:/3ds/Gravepowder/save.bin.tmp`,
   flush/close, then rename over `save.bin` so a crash can't half-write it.
3. Load: `fopen(...,"rb")`, read up to `GRAVE_SAVE_SIZE` bytes, call
   `grave_load_from_buffer`; on any failure keep the fresh init state.
4. Save on: zone completion, back-to-title, settings/pack changes, app exit.

Image contents: magic `"GPV2"`, version, size, FNV-1a checksum over every byte
after the checksum field, per-zone unlocked/cleared masks + clears + best time,
current zone (map cursor), camera-mode flag, settings passthrough (`invert_y`,
`touch_sens`, `muted`), owned-gun flags, loaded ammo, material totals, lifetime
kills / zone runs / play time. `grave_save_to_buffer` memsets the image so two
saves of the same state are **byte-identical**; `grave_load_from_buffer` fully
validates (magic, version, size field, checksum, mask bits) **before** touching
the game, so a rejected image leaves the game untouched. Loading also wipes
transient run state (pools/quest/spawner) and leaves the game on
`GAME_PHASE_TITLE` with `world.in_zone = false`, ready to travel.

The `GraveSettings` passthrough is optional: pass `NULL` on save/load if the app
does not want the core to persist its settings.

## Determinism / restart

- Fixed-step deterministic: one xorshift32 in `game->rng`, no time/OS calls.
  Same seed + same `GraveInput` tape → identical `GraveGame` (tested over 10k
  steps, across zone restarts and save round-trips).
- `grave_game_start_zone` resets `rng = game->seed`, pools, quest, run
  materials, timers and frame counter; inventory/lifetime stats/world node flags
  persist. A restart is a clean replay of the zone.
- `void grave_game_step(game, NULL, dt)` is safe (treated as zeroed input).

---

## Balance / tuning rationale (v0.2)

All numbers live in `source/core/data.c` (weapon table, zombie_table,
zone_table). Current values:

| thing | value |
|---|---|
| player | 100 hp, 4.2 m/s walk, 6.6 m/s sprint (stamina ~4 s, regen), no i-frames, no healing |
| weapons | flintlock 34 (2 body shots / 1 head-ish on a shambler), revolver 18×6, musket 55 (one-shots the little ones) |
| zombies | shambler 44 hp / 1.15 m/s / 5 dmg; runner 30 hp / 2.55 m/s / 4 dmg; brute 140 hp / 0.8 m/s / 16 dmg |
| melee | windup 0.38–0.75 s, then damage if still in reach; 1.2–2.0 s between swipes |
| drop cycle | per kill: A, B, A, (powder 60% / A) |

**Headless sim** (`tools/sim.c`, perfect-aim bot, fixed 1/60 steps, seed 2024):

- ASHEN MILL: quest complete in **85.6 s**, 40/40 kills, 130 shots / 115 hits,
  21 reloads, 25 run drops, 29 pickups, **100/100 hp** left (bot took no hits).
- Trail: ASHEN MILL cleared, BONE ORCHARD unlocked automatically.
- BONE ORCHARD: quest complete in **165.5 s**, 55/55 kills, 251 shots / 234 hits,
  41 reloads, 31 run drops (19 coins, 12 bone charms), 38 pickups, **80/100 hp**.
- Campaign (bot): ~4.2 min total — inside the 3–5 min v0.1/v0.2 target; a human
  with touch aim and loot detours will be slower.
- Idle player: dies in **26.7 s** (passive death is real).
- Save round-trip / corrupt-image rejection is covered by the host tests, not
  the sim.

Why these numbers:
- Weapon TTK keeps the three guns meaningfully different without armour stats:
  flintlock 2 body/1 head shambler, revolver 3 body, musket one-shots the little
  ones and is the brute opener.
- Kiting is the intended defence: every zombie is slower than the player's walk
  (runner 2.55 vs 4.2), so positioning wins fights; sprint is a burst escape.
- ASHEN MILL is a short first stop (40 kills) that teaches the loop; BONE
  ORCHARD is longer, runner-heavy and puts one brute in every wave (budget 64,
  55 kills) so it demands the kiting/aim skills the first zone taught.
- The pending-wave spawner keeps "1 brute per wave" exact and prevents the
  alive-cap from turning every freed slot into a brute (that made the orchard
  ~2 min longer in early testing).
- The drop cycle guarantees the collect quota: 55 orchard kills always drop
  ≥28 coins and ≥14 bone charms, so 10 + 6 can never soft-lock even if some
  drops expire (60 s on the floor) or the pickup pool fills.

## Known limitations / deliberate shortcuts

- No height/jump gravity (`velocity_y`/`on_ground` kept but always 0/true); prop
  collision is 2D footprints (a fence blocks like a wall).
- No player i-frames; several zombies can land swipes in the same step. Damage
  per swipe is flat.
- Hitscan volumes are approximations (box + head sphere), not skeletal.
- OLD MINE has a spec + unlock chain but reuses the mill layout (v0.3 work).
- Save images are C-layout based (fine for 3DS + little-endian hosts); the
  checksum is a corruption guard, not a security feature. No save encryption.
- Pools are hard caps; full pools silently skip drops/spawns instead of
  overwriting live entries.
- Pause, weapon/camera buttons, menus and SD I/O live in the app layer; the core
  only simulates.
