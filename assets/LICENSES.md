# Third-party model assets

Every model in `assets/models/` is either **CC0 (public domain)** or **CC-BY 3.0** and was downloaded
from [Poly Pizza](https://poly.pizza), which hosts free low-poly models with clear licences. Nothing
here is ripped from a commercial game. The in-repo models are the *sources*; the game ships
converted `.gpm` files produced by `tools/import_model.py` (see [`docs/assets.md`](assets.md)).

| File | Model | Author | Licence | Source |
|---|---|---|---|---|
| `assets/models/hero_medieval.glb` | Medieval (modular body parts: Body/Head/Legs/Feet + 24 animations) | Quaternius | CC0 | https://poly.pizza/m/y9KWOVG21R |
| `assets/models/zombie_animated.glb` | Animated Zombie (13 animations) | bachosoftdesign | CC-BY 3.0 | https://poly.pizza/m/xqEzosAVYX |
| `assets/models/gun_flintlock.glb` | Pistol | Quaternius | CC0 | https://poly.pizza/m/ldAlSSvuqT |
| `assets/models/gun_revolver.glb` | Revolver | — | CC-BY 3.0 | https://poly.pizza/m/fGBhJzY228W |
| `assets/models/gun_musket.glb` | Rifle | Quaternius | CC0 | https://poly.pizza/m/neEjwx9bBJ |

## Attribution requirements

CC0 assets need no credit (but we credit anyway, because it's polite and the README has a credits
section). **CC-BY assets must be credited** in redistributed builds and in the README/release notes:

- *Animated Zombie* by **bachosoftdesign** — CC-BY 3.0 — https://poly.pizza/m/xqEzosAVYX
- *Revolver* by its Poly Pizza author — CC-BY 3.0 — https://poly.pizza/m/fGBhJzY228W
- (also kept for reference/swap options, not currently shipped: *Furry boi v2* by Yogoshimo 2.0 —
  CC-BY 3.0 — https://poly.pizza/m/bprrVO3Jw0t)

If you swap in a different CC-BY model from this folder or from anywhere else, add a row to the table
above and to the README credits. `tools/import_model.py` writes the model's licence/author into the
`.gpm` header strings automatically when a `--credit` is passed, so the game can print an in-game
"models" page in the pause menu later.

## Provenance / re-download

The GLB sources are committed so the build is reproducible offline (CI does not download anything).
They were fetched on 2026-11-05 from the static URLs:

- hero: https://static.poly.pizza/3186b8e9-afd5-4d48-846c-b2b530cd23e2.glb
- zombie: https://static.poly.pizza/8181fd4f-facb-4b10-83c3-43852fd69376.glb
- flintlock: https://static.poly.pizza/169f634b-e753-4b59-9435-78e7e505a62e.glb
- revolver: https://static.poly.pizza/cdb95925-1338-4d49-b0f9-d2554722163c.glb
- musket: https://static.poly.pizza/da83f4f9-7a4e-4739-9033-79d688aa3b5e.glb
- furry boi (optional hero swap): https://static.poly.pizza/984c981a-a53f-4b50-b38a-97eb3651be1b.glb

Also downloaded for evaluation but **not committed** (too heavy / wrong style): the 20k-vert
Adventurer variants, Cowboy (static), Pirate Captain (no gun animations), several CC-BY rifles.

## Cost note

Raw GLBs are large (a few MB); the converted `.gpm` files that actually ship are much smaller
(~0.3–1 MB total) because only the parts we use are exported and animations are resampled.
