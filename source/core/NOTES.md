# Portable gameplay core — implementation notes (v0.1 "Ashen Mill")

Everything in `source/core/` is plain C99, no `<3ds.h>`, no `malloc` in the loop,
fixed pools from `include/grave.h`. The renderer / UI / audio layers only **read**
`GraveGame` (plus the camera product); the app drives input and owns the buttons.

```
source/core/game.c        the whole simulation (this is the implementation of include/grave.h)
source/core/data.c        ALL gameplay numbers: weapon table, zombie stats, zone quest/spawner tuning
source/core/core_internal.h  internal-only tuning constants + ZombieSpec/ZoneSpec (NOT public contract)
source/core/input.c       input helpers (unchanged)
tests/test_core.c         host unit tests: 2591 checks, deterministic, no 3DS headers
tools/sim.c               headless autoplay bot + balance report
```

`include/grave.h` was only **appended** to, at the end of two structs:
`Player.reload_weapon` and `GraveGame.seed`. Both are internal bookkeeping; the
front end can ignore them.

---

## Ownership split (matches the app layer contract)

The core consumes from `GraveInput`: `move_x`, `move_y`, `look_x`, `look_y`,
`fire` (held), `sprint` (held), `reload` (tap), `swap_weapon` (tap),
`toggle_camera` (tap).

Weapon switching and camera mode are supported **both** ways, because the app
does both: in-game X/Y arrive as edge taps (`input.swap_weapon` /
`input.toggle_camera`) and the core applies them; the pause/settings menus write
`game->player.weapon` / `game->player.camera_first_person` directly and the core
honours those values. The core never resets the weapon or camera mode by itself
(only `grave_game_start_zone` puts the camera back in third person, so the app
should restore a saved preference after `start_zone`). An app-side weapon change
during a reload cancels the reload (`Player.reload_weapon` bookkeeping) — no free
rounds.

`pause` is app-owned: the app freezes stepping (`a->paused`); `game->phase` may
stay `PLAYING` while paused. If the app does set `GAME_PHASE_PAUSED`, the core
no-ops until it is set back. `input.pause` and `input.interact` are reserved and
currently unused by the core (pickups are automatic in v0.1).

`game->phase` is authoritative: the core sets `GAME_PHASE_QUEST_COMPLETE` once
the quest is fully done and `GAME_PHASE_GAME_OVER` when hp hits 0. While either
of those (or `PAUSED`) is set, `grave_game_step` advances nothing.

`dt` is expected to be a fixed `1/60`; the core clamps anything outside
`(0, 0.25]` and can be called 0..N times per frame.

## Events (`game->events`)

- `grave_game_step` **ORs** `GRAVE_EVT_*` bits into `game->events`; it never
  clears the word. The app must do `ev = game->events; game->events = 0;` after
  every step and then dispatch to audio/UI. A bit is raised exactly once per
  occurrence and never re-raised on idle frames.
- `GRAVE_EVT_SHOOT` once per shot (weapon is `player.weapon`).
- `GRAVE_EVT_RELOAD` when a reload **starts** (manual or auto). There is no
  separate "reload finished" bit; completion is observable as
  `player.reload_timer` crossing 0 (`ammo[weapon]` jumps to the magazine).
- `GRAVE_EVT_EMPTY_GUN` dry trigger on an empty magazine.
- `GRAVE_EVT_HIT` once per shot that actually hit a zombie.
- `GRAVE_EVT_ZOMBIE_DIE` once on the killing hit (zombie is `ZOMBIE_DYING`).
- `GRAVE_EVT_PICKUP` once per pickup collected (any kind).
- `GRAVE_EVT_HURT` once per zombie swipe that lands on the player.
- `GRAVE_EVT_QUEST_DONE | GRAVE_EVT_ZONE_DONE` raised together on completion.
- No footstep/death bits: derive footsteps from `Player.anim_speed`; watch
  `phase` for game over / results.

## Camera product (read-only for the front end)

`grave_update_camera(game, aspect)` fills `game->camera_pos`,
`game->camera_look`, `game->camera_fov` and **ignores** `aspect` (aspect is
handled by the renderer's projection). The step function never reads these.

- Units: metres + radians. `camera_pos` / `camera_look` are world positions;
  `Mtx_LookAt(camera_pos, camera_look, up=(0,1,0))`.
- `camera_fov` is the **vertical FOV in degrees** (62 third person, 68 first
  person) — `C3D_AngleFromDegrees(camera_fov)`.
- `camera_look = aim point`, i.e. the direction `camera_look - camera_pos` is
  exactly the aim direction (crosshair = shot line). The third-person camera
  orbits at 3.2 m behind a pivot 1.55 m above the feet, 0.45 m over the right
  shoulder, and pulls itself in when it would clip a prop / the arena wall /
  the floor, so it can be closer than 3.2 m near geometry.
- `player.recoil` (radians) is already folded into the camera pitch; the
  renderer should not add it again.
- Camera mode changes take effect on the next `grave_update_camera` call.

## Hero

- `grave_hero_root(game, &pos, &yaw)`: feet position (y = 0) and facing yaw.
  yaw 0 = +z, positive turns right (towards +x).
- `grave_hero_walk_phase(game)`: `player.anim_time`, a running phase in radians
  for `sin()`-driven limb swing; `player.anim_speed` is 0 idle / 1 walk / ~1.35
  sprint, so the renderer can blend.
- `player.anim_time` advances at walk rate scaled by `anim_speed`; sprint is
  faster. Add any vertical bob in the renderer (the root stays on the ground).
- `player.firing`/`muzzle_flash` are the muzzle-flash timers (`muzzle_flash`
  counts down from 0.07 s); `player.recoil` is the camera kick.

## Zombies

`Zombie` fields are renderer-ready:

- `state`: `IDLE` (spawn notice delay) -> `CHASE` -> `ATTACK` (windup then
  swipe, then cooldown; stays in ATTACK while the player is in reach) or
  `STAGGER` (hit reaction) -> `DYING` (`death_time` 0..1 over 1.15 s, fall +
  fade) -> `DEAD` (immediately `active = false`, so the renderer just stops
  drawing it).
- `anim_time` runs continuously (walk/death cycles); `speed` is current m/s
  (0 while attacking/staggered); `yaw` faces the player.
- `attack_timer` meaning: in `ATTACK` it is seconds until the next swipe lands
  (_windup_) or until the next swing (_cooldown_); use `anim_time` for the pose.
- `anim_time`, `yaw` and `death_time` are the intended animation drivers;
  `hp/max_hp` drive overhead bars if wanted.
- Hitbox model is body box (`radius` wide, `body_height - 0.26` tall) plus a
  head sphere at `body_height - 0.13` (r 0.13). A "head-ish" hit does 1.5x
  damage — `GRAVE_EVT_HIT` fires either way; if you want a headshot speck,
  compare the tracer end height to the zombie's head height.
- `zombies_alive` includes dying bodies until the fade finishes (~1.15 s);
  radar blips may want to skip `DYING`/`DEAD`.
- Spawns are validated against props, so `active` zombies are never inside
  geometry; spawn points are on the arena perimeter / gate, at least 7 m from
  the player.

## Pickups

- `Pickup.pos.y` is a base height (0.35 m); add the bob in the renderer from
  `bob_time` (radians, advancing). `life` counts down from 60 s; collect happens
  automatically within 1.55 m of the player (no interact key in v0.1).
- `kind`: coin / sinew go to `material_coins` / `material_sinew` (quest
  materials); powder tops the **current** weapon up by 2 rounds (clamped to
  magazine), i.e. it can instantly load a flintlock round — intended as a small
  lucky break.
- The drop cycle is deliberately deterministic-ish: kills 1,2,3,4,5... drop
  coin, sinew, coin, (powder 60% / coin 40%), repeating. With the kill goal this
  guarantees the 10 coin + 5 sinew collection quota can never soft-lock.

## Props

- `Prop.pos` is the **centre of the base**; the box is
  `[pos.x ± half.x] × [pos.y .. pos.y + 2*half.y] × [pos.z ± half.z]`.
- `half` are collision half extents. Collision treats every prop as a
  full-height XZ footprint (the player never jumps in v0.1) and ignores `yaw`;
  the renderer can freely rotate the drawn box by `yaw`.
- `color` = `(uint8_t)kind` right now, always 0..7; the renderer can map
  kind -> palette shade (crate / barrel / fence / tombstone / deadtree / well /
  gate). `prop_count` is 90 for ASHEN MILL, deterministic (fixed hand-authored
  layout — no RNG in layout).
- Arena is 40 × 40 m; player bounds are ±19.2 m (`GRAVE_ZONE_BOUND_PAD`).
  Player spawns at `(0, 0, 15.5)` facing `-z` (gate at its back).

## Quest (v0.1)

`game->quest` is a staged quest; the HUD can render it generically:

1. `QUEST_CLEAR_ZONE`: `target = spawn_kill_goal` (56), `progress = zombies_killed`
   (clamped), `material = DROP_NONE`. Text: "CLEAR THE YARD".
2. `QUEST_COLLECT`, `material = DROP_COIN`, `target = 10`, `progress = coins`.
3. `QUEST_COLLECT`, `material = DROP_SINEW`, `target = 5`, `progress = sinew`.
4. `QUEST_DONE` + `GAME_PHASE_QUEST_COMPLETE` + events. Not immediately left;
   the app decides (results screen -> restart zone from title).

Stages can hop multiple at once if the player already has the goods; check them
in a loop each frame rather than assuming one transition per step.

## Weapon semantics

- `ammo[i]` = rounds currently loaded (no reserve pool in v0.1). Reload always
  fills the magazine after `reload_time`; the cost is the reload time, not ammo
  scarcity.
- Auto-reload starts when the magazine empties (including a dry trigger). `B`
  can start a reload early.
- Fire cooldowns / reload timers / spread / recoil all come from `data.c`.
  Recoil kicks the camera and half of it is carried into the next shot's aim —
  the shot that causes the kick is not affected by it.
- Hitscan: props stop shots (fence is waist-high cover); zombie hit test is a
  body box + head sphere. No damage fall-off. `ShotTrace.kind`: 0 = tracer
  (life 0.06 s, `start` = muzzle, `end` = hit point / max range), 1 = impact
  spark (life 0.22 s, `start == end`). Draw while `life > 0`; suggested alpha
  `life / 0.06` (tracer) and `life / 0.22` (impact).

## Determinism / restart

- Fixed-step deterministic: one xorshift32 in `game->rng`, no time/OS calls.
  Same seed + same `GraveInput` tape -> identical `GraveGame` (tested over 10k
  steps and across zone restarts).
- `grave_game_init(game, seed)` stores the seed in `game->seed`; every
  `grave_game_start_zone` resets `rng = seed`, all pools, quest, materials,
  events, frame, timers, player. A restart from the title is a clean replay.
- `void grave_game_step(game, NULL, dt)` is safe (treated as zeroed input).

---

## Balance / tuning rationale (v0.1)

All numbers live in `source/core/data.c` (weapon table, zombie_table,
zone_table). Current ASHEN MILL tuning:

| thing | value |
|---|---|
| arena | 40 × 40 m, player bounds ±19.2 m |
| spawn budget / kill goal | 68 spawned max / 56 kills for the clear step |
| alive cap / wave interval | 8 concurrent / 2.8 s between waves |
| collect quota | 10 old coins + 5 rot sinew |
| zombie stats | shambler 44 hp / 1.15 m/s / 5 dmg; runner 30 hp / 2.55 m/s / 4 dmg; brute 140 hp / 0.8 m/s / 16 dmg |
| zombie melee | windup 0.38–0.75 s, then damage if still in reach; 1.2–2.0 s between swipes |
| player | 100 hp, 4.2 m/s walk, 6.6 m/s sprint (stamina ~4 s sprint, regen), no i-frames, no healing |
| weapons | flintlock 34 (2 body shots on a shambler, 1 head shot), revolver 18×6 (3 body shots), musket 55 (one-shots the little ones) |

**Headless sim result** (`tools/sim.c`, perfect-aim bot, fixed 1/60 steps):
third person = first person = **56 kills, quest complete in 135.5 s**, 208 shots /
198 hits, 33 reloads, 30 drops, 36 pickup collections, **52/100 hp left**;
idle player who never fights **dies at 26.7 s**. The bot is a ceiling, not an
average player: touch aim, dodging and loot detours make a human run longer
(roughly 3–4 min), which is the v0.1 target. The bot survives with about half
its health, so there is slack for an imperfect aimer but the zone still kills a
passive player.

Why these numbers:
- Weapon TTK: shambler dies to 2 flintlock body shots / 1 lucky-slightly-high
  head shot / 3 revolver rounds / 1 musket; brute is an "empty the cylinder"
  wall. This keeps the three guns meaningfully different without armour stats.
- Wave pacing (8 alive, 2.8 s) keeps pressure constant without cornering the
  player; a 10-alive cap made the bot die in a death spiral, 8-9 lets kiting
  work.
- Kiting is the intended defence: every zombie is slower than the player's walk
  (runner 2.55 vs 4.2), so positioning wins fights. Sprint is a burst escape.
- The drop cycle guarantees the collect step: 56 kills always drop >=28 coins
  and >=14 sinew on the rotating cycle, so 10 + 5 is reachable even if some
  drops expire (60 s on the ground) or the pickup pool fills.
- `zombies_alive` counts dying bodies for ~1.15 s; spawning uses a hard 8 cap
  (`max_alive`) and the pool cap is `MAX_ZOMBIES` = 64 (never reachable in this
  zone, kept as a safety rail).
- Player never starts inside a prop; layout hand-placed with the spawn area
  clear, zombie spawn positions rejection-sampled against props/each other and
  the player (min 7 m).

## Known limitations / deliberate v0.1 shortcuts

- No height/jump gravity: `player.velocity_y`/`on_ground` are kept but always
  0/true; prop collision is 2D footprints (a fence blocks like a wall).
- No player i-frames; several zombies can land swipes in the same step. Damage
  per swipe is flat (no head/body split on the receiving side).
- Hitscan hit volumes are approximations (box + head sphere), not skeletal.
- Zones other than ASHEN MILL reuse the same arena layout (v0.2 placeholder).
- `MAX_ZOMBIES`/`MAX_PICKUPS` are hard pool caps; when a pool is full, drops or
  spawns are silently skipped rather than overwriting live entries.
- Pause lives in the app layer; the core only simulates. Weapon/camera buttons
  are handled by the core as edge taps and can also be driven by direct player
  field writes from the menus.
