# Portable gameplay core — implementation notes (v0.3, "The Foreman")

Everything in `source/core/` is plain C99, no `<3ds.h>`, no `malloc` in the loop,
fixed pools from `include/grave.h`. The renderer / UI / audio layers only **read**
`GraveGame` (plus the camera product); the app drives input, owns the buttons and
owns the SD card file I/O.

```
source/core/game.c        the simulation (implements include/grave.h)
source/core/data.c        ALL gameplay numbers: weapons, zombies, zones, shop
source/core/core_internal.h  internal-only tuning + ZombieSpec/ZoneSpec (NOT public)
source/core/input.c       input helpers (unchanged)
tests/test_core.c         host unit tests: 3958 checks, deterministic, no 3DS headers
tools/sim.c               headless 3-zone campaign (shop + `--plain` control)
```

v0.3 is **purely additive** in `include/grave.h`; the v0.1/v0.2 contract is
untouched apart from the save version bump:

- enums: `ZOMBIE_FOREMAN` (before `ZOMBIE_COUNT`), `PROP_TIMBER`, `PROP_CART`,
  `PROP_ORE`, `PROP_LANTERN` (before `PROP_KIND_COUNT`),
  `GRAVE_EVT_BOSS_SPAWN (1<<9)`, `GRAVE_EVT_BOSS_DIE (1<<10)`,
  `GRAVE_EVT_WORLD_COMPLETE (1<<11)`
- new types: `ShopItem`, `ShopLevelField`, `ShopDesc`, `ShopItemSpec`
- appended struct fields:
  - `Player`: `weapon_tune[WEAPON_COUNT]`, `vitality_level`, `reload_tune`,
    `reload_duration`
  - `GraveGame`: `total_shots`, `total_hits`, `boss_spawned`, `boss_alive`,
    `boss_defeated`, `boss_hp`, `boss_max_hp`, `world_complete`
- new API: `grave_zone_half_extent`, `grave_shop_item`, `grave_shop_level`,
  `grave_shop_can_buy`, `grave_shop_buy`
- save: `GRAVE_SAVE_VERSION 3`, magic `"GPV3"`, `GRAVE_SAVE_SIZE 108`
  (v2 images are rejected cleanly by the version field)

Memory impact: `sizeof(GraveGame)` went 13616 → **13668 B** (+52 B; +24 Player
fields, +28 GraveGame fields incl. padding) — negligible. `SaveData` 92 → 108 B.
Combat/collision loops are unchanged; the boss is just a bigger `Zombie`.

---

## Gameplay changes from v0.2 (front-end relevant)

- **Inventory now carries between zones**: loaded ammo per gun and current HP
  are preserved by `grave_game_start_zone` (a dead player retrying a zone gets
  full HP). The trader is therefore the intentional heal/refill: medkits heal,
  ammo refills, vitality raises max HP. (v0.2 silently refilled both at zone
  start; that made two shop items pointless.)
- **Quest completion pays out** (ZoneSpec rewards, added to the material
  inventory the step the zone is cleared): ASHEN MILL +70c/+12s,
  BONE ORCHARD +140c/+10s/+10ch, OLD MINE +200c/+14ch. This is what funds the
  trader; without it the specified prices are unaffordable mid-campaign.
- **The Foreman**: OLD MINE's boss spawns once per run when 40 normal zombies
  have been killed (`GRAVE_EVT_BOSS_SPAWN`). When he arrives the remaining
  normal zombies fall (DYING) so the finale is a boss duel, and the mine spawner
  pauses while `boss_alive`. The mine quest's final `QUEST_DONE` is **gated on
  `boss_defeated`**; killing the Foreman raises `GRAVE_EVT_BOSS_DIE`, drops 5
  coins + 2 bone charms, and does **not** count toward the normal kill goal
  (it does add to `total_kills`). Boss kills fire the same hitscan body/head
  volumes, just at boss scale; it never staggers.
- **World completion** (`world_complete` + `GRAVE_EVT_WORLD_COMPLETE`) is set
  when OLD MINE is cleared; the three nodes then read cleared[1 1 1].
- **Accuracy**: `total_shots` increments per fired pellet/hitscan, `total_hits`
  per shot that damaged a zombie (a shot hits at most one zombie). Both saved.
- **OLD MINE is a real zone now** with a mine-works layout built from the four
  new `PropKind`s plus the existing fence/gate/crate/barrel kinds; OLD MINE no
  longer reuses the mill layout.

## OLD MINE + Foreman tuning

| thing | value |
|---|---|
| arena | 40 × 40 m (`grave_zone_half_extent == 20`), tighter adit/corridor layout, 133 props |
| spawner | budget 55, kill goal 45, alive cap 7, wave 2.6 s, 1 brute/wave, runner-heavy |
| quest | kill 45 → 8 old coins → 3 bone charms → (Foreman dead) → DONE |
| Foreman | 1680 HP (12× brute; 3 "thick layers" = 3 HP-bar segments), speed 0.50 m/s, 30 dmg swipe, 1.2 s windup / 2.8 s cycle, radius 1.2 m, height 3.4 m, no stagger |
| boss spawn | once, at 40 normal kills, `GRAVE_EVT_BOSS_SPAWN`; rabble scatters; spawner pauses |

The boss uses the ordinary zombie model/pool (renderer just scales it up). UI
draws `boss_hp / boss_max_hp`; when `boss_alive` is 0 the bar is hidden.

## Trader (shop)

`grave_shop_can_buy` is false while `world.in_zone` (trail-only trader), at max
level, for a consumable that would do nothing (full HP / full guns), or when
unaffordable. `grave_shop_buy` returns false and changes nothing otherwise, and
applies the effect immediately. `ShopItemSpec` carries name, `ShopDesc` id,
per-level cost tables (index = current level), max level (0 = repeatable), the
level-field selector and the weapon index for tunes.

| item | price | max | effect |
|---|---|---|---|
| AMMO | 25 c | repeatable | refill all owned guns to full magazine |
| TUNE (per gun) | L1 60c+2ch, L2 120c+4ch, L3 220c+7ch | 3 | +15% damage per level (multiplicative with headshot ×1.5) |
| VITALITY | 40c+1s per level | 5 | +10 max HP per level, heals by the gained amount |
| RELOAD | 50c+2s per level | 3 | −8% reload time per level (floor 0.20 s) |
| MEDKIT | 30 c | repeatable | heal 50% of max HP instantly |

Damage = `weapon_spec.damage × (1 + 0.15 × weapon_tune[weapon])`
Reload = `weapon_spec.reload_time × (1 − 0.08 × reload_tune)`, ≥0.20 s;
`Player.reload_duration` exposes the effective duration for the HUD progress
bar (the old `reload_timer / spec.reload_time` ratio is wrong once tuned).
Max HP = `GRAVE_PLAYER_MAX_HP (100) + 10 × vitality_level`.

## OLD MINE layout / new props

- `Prop.color == (uint8_t)kind` as before; new indices: 7 TIMBER, 8 CART,
  9 ORE (glowing crystal cluster), 10 LANTERN. Collision is the same
  half-extent XZ box model (yaw only affects rendering).
- Layout: palisade + south gate, a 3.6 m-wide central adit framed by timber
  supports every 5 m, ore/rock walls with two side chambers (timber stacks,
  carts, crystal seams), lantern posts along the adit. 133 props, deterministic,
  player spawn clear, zombie spawn points validated as usual.

## Events (v0.3 additions)

Everything from v0.2 still holds (the core ORs bits once per occurrence, the app
clears the word after dispatch). New bits: `BOSS_SPAWN`, `BOSS_DIE`,
`WORLD_COMPLETE` (appended after `EMPTY_GUN`), so the valid mask is now 0xFFF.

## Save data v3

Same fixed 108-byte portable image, magic `"GPV3"`, version 3, FNV-1a checksum
over every byte after the checksum field. Appended fields: `weapon_tune[3]`,
`vitality_level`, `reload_tune`, `world_complete`, `shots_fired`, `shots_hit`.
Round-trip is byte-deterministic; old v2 saves are rejected by version before
any game state is touched (app keeps fresh init). Save/load also carries the
lifetime accuracy counters. `grave_game_load_*` leaves the game on the trail
(`phase = TITLE`, `world.in_zone = false`) with HP/upgrades restored.

## Balance / tuning rationale (v0.3)

All numbers live in `data.c`. The v0.3 changes were driven by the sim:

- **Trader economy**: quest payouts (above) were added because the specified
  prices (60c first tune etc.) cannot be met from drops alone early on. The sim
  buys 2× vitality after the mill, then a medkit + revolver tune L1 + 2 more
  vitality before the mine; ~6 purchases over the campaign.
- **Foreman fight**: the mine spawner pauses and the rabble scatters at spawn;
  without that the 1-v-1 boss + 7 adds in the narrow mine killed the bot every
  run. With focus, the bot kites the slow boss and wins.
- **Carry HP/ammo**: makes vitality/medkit/ammo purchases meaningful and gives
  the campaign a roguelite attrition curve; plain runs end at 11/100 HP on the
  last zone, shop runs at 117/140.

### Headless sim (perfect-aim bot, fixed 1/60, seed 2024)

| run | Ashen Mill | Bone Orchard | Old Mine | Campaign | Foreman | End HP |
|---|---|---|---|---|---|---|
| with trader | 85.6 s | 165.5 s | 247.2 s | **498.2 s** (~8.3 min) | spawn 163.5 s, kill 229.1 s (**65.7 s fight**) | 117/140 |
| `--plain` (no shop) | 85.6 s | 165.5 s | 267.2 s | **518.1 s** (~8.6 min) | spawn 180.8 s, kill 256.7 s (**75.9 s fight**) | 11/100 |

Both runs: 141 lifetime kills, ~80% accuracy, world_complete = 1,
cleared [1 1 1]. Shop run is ~20 s faster and much safer; the plain run is the
control showing the upgrades matter. The bot uses medkits/vitality to survive
the orchard and kites the Foreman (no boss hits taken in the duel).

Trade-off: a 65–76 s boss duel is long for a handheld finale but it is the
12×-brute HP budget; if it feels long in play, lowering `ZOMBIE_FOREMAN.hp`
in `data.c` is the single tuning knob (it is a data-only change).

## Known limitations / deliberate shortcuts

- No height/jump gravity; prop collision is 2D footprints (yaw render-only).
- No player i-frames; several zombies can land swipes in the same step.
- Hitscan volumes are approximations (box + head sphere), not skeletal.
- The Foreman is "three layers" as three HP-bar segments, not armour phases.
- Shop availability is the single `!world.in_zone` check (trail/WORLD screen);
  there is no separate trader page state in the core.
- Pause, weapon/camera buttons, menus and SD I/O live in the app layer; the core
  only simulates. Save images are C-layout based; checksum is a corruption
  guard, not security.
- Pools are hard caps; full pools silently skip drops/spawns.

---

# v0.4 "Steady Hands" — controls / feel (append-only)

Motivation: on real hardware the left stick moved the player the "wrong" way,
yaw drag was mirrored, there was no way to turn around, and aiming/gyro were
missing. Root cause: the core's screen-right basis disagreed with the
renderer's camera basis (`fwd × up`), so strafe and yaw drag were both mirrored.

## Screen-space conventions (the one thing to remember)

```
aim/forward fwd(yaw)   = ( sin yaw, 0,  cos yaw)
screen-right flat_right(yaw) = (-cos yaw, 0,  sin yaw)   = fwd × up
```

At yaw 0 the player faces +z, so **screen-right is world −x**. Positive yaw
rotates towards +x, which is screen-LEFT. Everything input-facing follows:

- stick `move_x>0` = screen-right; camera over-the-shoulder pivot offsets
  screen-right (TP camera now shifts to −x at yaw 0).
- touch `look_x>0` = drag/turn right on screen → `yaw -= look_x * 2.2`.
- `look_y>0` = look down → `pitch -= look_y * 1.6` (unchanged).
- gyro `gyro_yaw>0` rad/s = turn right on screen → `yaw -= gyro_yaw * dt`;
  `gyro_pitch>0` = look up → `pitch += gyro_pitch * dt`.
- The renderer needed **no change**: it consumes `camera_pos/look/fov` and the
  core's `flat_right` now matches its `fwd × up` basis.

## New append-only API (`include/grave.h`)

- `GraveInput += bool aim; float gyro_yaw; float gyro_pitch;`
  - `aim` = the L button held. `gyro_*` = rad/s supplied by the app's motion
    read (app applies gyro_mode/sens and the actual hardware scaling first; the
    core only integrates rates). Positive conventions above.
- `Player += bool aiming; float vel_x, vel_z;`
  - `aiming` mirrors held aim (UI/animation/VFX); `vel_*` is the internal
    horizontal velocity state for the acceleration model (UI can ignore).
- `GraveSettings += uint8_t gyro_mode; uint8_t gyro_sens;`
  - `GraveGyroMode { ON=0 (default), OFF=1, INVERT=2 }`,
    `GraveGyroSens { MED=0 (default), LOW=1, HIGH=2 }` (append-only enums).
  - Persisted in `SaveData.reserved[0]`/`[1]`: **save version stays 3 and the
    image stays 108 bytes**; pre-v0.4 images (zeroed reserved) load as ON/MED,
    junk values sanitise to ON/MED.
- No new functions. Renderer/app read `player.aiming` / `camera_fov` as before.

## Aiming effects (`core_internal.h` constants)

| constant | value | effect |
|---|---|---|
| `GRAVE_AIM_MOVE_SCALE` | 0.55 | move target speed ×0.55 while aiming; sprint disabled |
| `GRAVE_AIM_SPREAD_SCALE` | 0.35 | shot spread ×0.35 while aiming |
| `GRAVE_AIM_RECOIL_SCALE` | 0.70 | weapon kick ×0.70 while aiming |
| `GRAVE_AIM_FOV` | 50.0° | vertical FOV in **both** camera modes while aiming |
| `GRAVE_AIM_TP_DISTANCE` | 2.0 m | third-person camera distance while aiming (shoulder + wall pull-in kept) |
| `GRAVE_AIM_ASSIST_RAD` | 0.05 rad (~2.9°) | aim-assist cone |
| `GRAVE_MOVE_ACCEL` / `GRAVE_MOVE_DECEL` | 14 / 18 m/s² | velocity ramp to/from the wish speed |
| `GRAVE_RECOIL_RECOVERY` | 11/s (was 9) | slightly faster recoil recovery |

- **Aim assist**: when `aiming` and the direct hitscan ray misses, the nearest
  zombie whose direction is within `GRAVE_AIM_ASSIST_RAD` of the aim ray, is
  inside weapon range and has a clear line of fire (props checked) takes the
  hit as a normal body hit (no headshot bonus). Hip-fire never assists.
- **Movement feel**: horizontal velocity approaches the target velocity at
  14 m/s² (accelerating) / 18 m/s² (slowing or turning); collision/position
  resolution is unchanged, and velocity is re-derived from the resolved motion
  so walls do not wind it up. Walk/sprint speeds and stamina are unchanged.
- Enemy/quest/shop/save tuning was **not** touched in v0.4.

## Verification (v0.4)

- Host tests: **4024 checks, 0 failed** (`-Wall -Wextra -pedantic` clean).
  New suites: axis/gyro geometry, aiming (speed/FOV/camera/recoil/spread/aim
  assist incl. range + LOS negatives), gyro settings round-trip,
  pre-v0.4-image compat and junk-value sanitising; stress now feeds aim/gyro
  and checks velocity/pitch invariants.
- 3DS build: clean `build3ds.cmd`, warning-free.
- `sizeof(GraveGame)` 13668 → **13680 B** (+12 B: aiming bool + velocity pair);
  `SaveData` 128? no — **108 B unchanged**; `GraveSettings` 6 B;
  `GraveInput` 32 B.

### Headless sim (seed 2024, fixed 1/60, bot updated for v0.4)

| run | Ashen Mill | Bone Orchard | Old Mine | Campaign | Foreman | End HP |
|---|---|---|---|---|---|---|
| with trader | 83.2 s | 195.1 s | 319.2 s | **597.5 s** (~10 min) | spawn 210.1 s, kill 294.7 s (**84.6 s fight**) | 104/140 |
| `--plain` (no shop) | 83.2 s | 195.1 s | 273.2 s | **551.5 s** (~9.2 min) | spawn 162.3 s, kill 254.7 s (**92.4 s fight**) | 69/100 |

Both runs clear all 3 zones incl. the Foreman, world_complete = 1,
cleared [1 1 1], 0 deaths, no boss hits taken (kiting). Bot changes for v0.4:
`look_x` sign flipped for the new yaw convention (steering fixed), pre-emptive
retreat band (4.6 m / 2+ crowded) because acceleration ramps kiting speed,
panic widen at <45% HP, boss-only hold-aim (aim walk cap would let runners
catch the bot mid-horde). The sim zone cap was raised to 8 simulated minutes so
slow-but-alive kiting runs are not called timeouts; the longer times are the
bot's cautious play, not a game-tuning change.

## Front-end notes (engine-3ds)

- L = aim, R = fire: app sets `input.aim` / `input.fire`; no core change needed.
- Gyro: read sensor rates once per frame, apply mode/sens (ON/OFF/INVERT;
  LOW/MED/HIGH) in the app, then fill `gyro_yaw`/`gyro_pitch` in rad/s with the
  sign conventions above. Store the two setting bytes in `GraveSettings` and
  pass it to save/load (they ride in `reserved`; no save version bump).
- Goal/scoped camera comes free from `grave_update_camera` (`camera_fov` 50
  while aiming; TP distance 2.0 m) — do not hard-code FOV in the renderer.
- `player.aiming` can drive reticle/VFX; reload HUD keeps using
  `player.reload_duration`.

---

# v0.5 "Cute & Deadly" — aim assist setting + magnetism (append-only)

Real-hardware feedback: gyro felt bad, text hard to read, and hitting zombies
was hard. v0.5 helps the shooting (the text/UI and "cuter" art/animation work
happens in the app/renderer, not the core). Enemy/quest/shop tuning untouched.

## New append-only API (`include/grave.h`)

- `GraveSettings += uint8_t aim_assist;`
- `GraveAimAssist { OFF=0, NORMAL=1 (default), STRONG=2, COUNT }`
- `GraveGame += uint8_t aim_assist;` — the active mode mirrored from settings by
  the app (`game.aim_assist = settings.aim_assist` after load/settings change);
  `grave_game_init` defaults it to NORMAL and the core sanitises junk values on
  every use, so an app that never writes it still behaves like NORMAL.
- `int grave_aim_target(const GraveGame *game);` — read-only reticle query:
  index of the zombie closest to the aim ray inside the active assist cone,
  weapon range and with a clear line of fire; -1 otherwise. Requires
  `player.aiming`, consumes no RNG, changes no state. Renderer should use it to
  colour/lock the reticle (NORMAL = green-ish, STRONG = brighter) and app UI can
  show the mode row as OFF / NORMAL / STRONG.

### Persistence

`SaveData.reserved[2]` stores `mode + 1` (so a pre-v0.5 image's zero byte means
"unset" and loads as NORMAL; OFF saves as 1). Save version stays 3, image stays
108 B. Junk reserved values sanitise to NORMAL.
**Fresh settings caveat:** zero-initialised `GraveSettings` means OFF (enum 0),
so the app must explicitly set `aim_assist = GRAVE_AIM_ASSIST_NORMAL` when it
creates first-run default settings (the game state itself defaults to NORMAL).

## Aim assist + magnetism (`core_internal.h`)

| constant | value | meaning |
|---|---|---|
| `GRAVE_AIM_ASSIST_OFF_RAD` | 0 | OFF: cone hit + magnetism disabled |
| `GRAVE_AIM_ASSIST_NORMAL_RAD` | 0.06 rad (~3.4°) | NORMAL cone |
| `GRAVE_AIM_ASSIST_STRONG_RAD` | 0.11 rad (~6.3°) | STRONG cone |
| `GRAVE_AIM_MAGNET_NORMAL` | 1.6 rad/s | NORMAL magnet angular speed |
| `GRAVE_AIM_MAGNET_STRONG` | 2.8 rad/s | STRONG magnet angular speed |
| `GRAVE_HITBOX_RADIUS_SCALE` | 1.20 | zombie body hitscan half-width/depth |

- **Cone assist (firing)**: when aiming and the direct hitscan ray misses, the
  nearest zombie whose aim point is within the mode's cone of the *actual shot
  ray*, in weapon range and with prop LOS, is hit as a normal body hit (no
  headshot bonus). Hip-fire never assists, OFF never assists.
- **Magnetism**: when aiming and a target is in cone, the core rotates yaw/pitch
  toward its aim point (chest at `body_height*0.55`) at the mode's angular
  speed. `player_look` runs first each step, so player touch/gyro input always
  dominates; the magnet step is capped by the angular error (never overshoots)
  and scaled by `error / NORMAL_RAD` (0.25..1) so small corrections stay gentle.
  STRONG is always stronger than NORMAL for the same error.
- **Hitscan hitbox**: the zombie *body* box half-width/depth is multiplied by
  `GRAVE_HITBOX_RADIUS_SCALE` (20% wider) in `hitscan()`; the head sphere is
  untouched, so headshot logic/rate is unchanged. This is the "hits connect
  more" half of the fix; hip spread was left as-is (sim balance held).

## Verification (v0.5)

- Host tests: **4074 checks, 0 failed** (`-Wall -Wextra -pedantic` clean).
  New suites: cone radius per mode via `grave_aim_target` (NORMAL vs STRONG vs
  OFF, behind, out-of-range, LOS-blocked, dying, not-aiming, read-only hash),
  magnetism convergence/no-overshoot/STRONG>NORMAL/OFF/hip/OFF-player-push,
  assist firing per mode (NORMAL miss, STRONG hit, OFF miss, hip never),
  widened hitbox inside/outside cases, aim-assist settings round-trip +
  pre-v0.5 compat + junk sanitising + core default.
- 3DS build: clean `build3ds.cmd`, warning-free (see artifact hashes in the final
  report).
- Sizes: `GraveGame` **13680 B unchanged** (the byte sits in existing padding);
  `GraveSettings` 7 B; `SaveData` 108 B unchanged.

### Headless sim (seed 2024, fixed 1/60, bot unchanged except it benefits)

| run | Ashen Mill | Bone Orchard | Old Mine | Campaign | Foreman | End HP |
|---|---|---|---|---|---|---|
| with trader | 86.9 s | 166.0 s | 251.1 s | **503.9 s** | spawn 156.4 s, kill 230.5 s (**74.1 s fight**) | 82/140 |
| `--plain` (no shop) | 86.9 s | 166.0 s | 288.4 s | **541.2 s** | spawn 206.9 s, kill 272.5 s (**65.7 s fight**) | 48/100 |

Both runs clear all 3 zones incl. the Foreman, world_complete = 1,
cleared [1 1 1], 0 deaths. The wider body box and the boss-duel magnetism make
the bot land more shots; times stay in the same band as v0.4.

## Front-end notes (engine-3ds / renderer, v0.5)

- After load or changing the settings row:
  `game.aim_assist = settings.aim_assist;` (values 0/1/2). The core query and
  magnetism read this mirrored byte.
- Use `grave_aim_target(&game)` for the reticle: -1 = no target (hip firing,
  OFF, or nothing in cone), >= 0 = zombie index you can tint by
  `game.aim_assist`.
- No other input changes; aim assist only applies while the L aim button is
  held. Fresh-run default settings should set `GRAVE_AIM_ASSIST_NORMAL`.


