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
