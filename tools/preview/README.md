# tools/preview - host UI/app preview & verification harness

Headless **bottom-screen** preview harness for Gravepowder. It drives the real
front-end scene state machine and the real portable core on a PC, records the
citro2d draw list of every UI page, replays it into PNG layout mockups, and
reports layout/legibility issues plus a comparison against the committed
Azahar screenshots.

It is **not** part of the 3DS build and it does **not** render the 3D top
screen (that is source/render3d.c on citro3d/PICA, out of scope here). The top
screen is verified from Azahar captures during the release pass; this harness
owns bottom-screen truth + the app/UI contract coverage.

## Build & run

Host compiler: `zig cc` is what CI uses (any C99 compiler works).

```sh
# from the repo root
make -C tools/preview CC="D:/tools/zig/zig.exe cc"        # build preview.exe
make -C tools/preview CC="D:/tools/zig/zig.exe cc" run    # tour + mockups + checks
make -C tools/preview clean
```

`make run` = `preview.exe --scenario all --out out` followed by
`python render_ui.py --out out --baselines ../../docs/screenshots`.

The replay/checker needs Python 3 with Pillow (the repo's host tooling already
uses Python for `tools/import_model.py`). The system font is approximated with
a local truetype font (Segoe UI / Arial / Calibri); text metrics and the
baseline comparison are advisory, not golden-image tests.

## Scenarios

| `--scenario` | Captures |
|---|---|
| `all` (default) | full continuous tour: title -> HUD -> pause/settings/save/wardrobe -> zone-clear -> trail/shop -> per-zone maps + boss bar + ending -> game-over |
| `title` | no-save prompt, has-save CONTINUE/NEW GAME, NEW GAME confirm |
| `hud` | AREA / WORLD / PACK pages, tab slide, reload + low-HP strip states, **v0.5 mascot moods (happy/worried/pale), event banner and touch-press ring** |
| `pause` | pause, settings, save sub-page (normal + erase-armed), wardrobe + reload |
| `trail` | zone-clear page, TRAVEL ON trail pages, SHOP page + purchase toast |
| `zones` | travel confirmation dialog, BONE ORCHARD / OLD MINE AREA maps, boss bar, campaign ending |
| `gameover` | death -> game-over page |

v0.5 adds five state captures on the AREA tab: `hud_mascot_happy`,
`hud_mascot_worried`, `hud_mascot_pale`, `hud_banner` and `hud_press`
(transient press-ring juice is cleared from all other captures so page
snapshots stay clean and reproducible).

`preview.exe` prints one line per capture:

```
[capture] page=hud_area          primitives= 332 text= 28 -> out/hud_area.txt
```

## Outputs (in `out/`, git-ignored)

| Path | Meaning |
|---|---|
| `<page>.txt` | recorded citro2d draw list for that page |
| `<page>_bottom.png` | 320x240 mockup (the real bottom-screen resolution) |
| `<page>_2x.png` | 2x nearest-neighbour view for review |
| `diff_<page>_<baseline>.png` | amplified difference vs a committed baseline |
| `check_report.txt` | layout + contrast findings |
| `preview_run.log` | stdout log when captured by the release scripts |

`render_ui.py` checks per page:
- text bounds are strict; decorative shapes clipped at the screen edge are
  allowed up to 8 px (the v0.5 mascot deliberately peeks up from the bottom
  edge, with its base shadow off-screen);
- text that crosses the HUD strip (0-33 px) or tab bar (36-58 px) bands;
- text-vs-text collisions (outline/shadow copies and panel-covered pairs are
  ignored) and per-page text extents with approximated citro2d metrics;
- panel containment on title/settings/pause/save/wardrobe/result pages (a
  control or panel must not cut through a text line);
- low-contrast text warnings (text hidden under a banner/toast/panel is skipped);
- **readability policy (v0.5+)**: non-decorative labels/body copy must use at
  least `MIN_READABLE_SCALE` (0.335); smaller text is reported as `[read]`
  (advisory, does not fail the layout gate). Tiny decorative stamps (version
  string, homebrew credit) and single-letter compass glyphs are exempt;
- text using >=95 % of its panel width is reported as `[tight]`;
- baseline comparison against `docs/screenshots/*.png` composites: the bottom
  screen is the 320x240 crop at `(40,248)` of the 400x488 README image, reported
  as mean absolute error + block mismatch count + a diff image.

Exit status is non-zero when the capture run fails or a **layout** issue is
found; contrast warnings, `[read]`, `[tight]` and baseline deltas are advisory
(the committed screenshots are from v0.2, so later pages legitimately differ).

## Files

| File | Purpose |
|---|---|
| `stub/3ds.h`, `stub/citro2d.h`, `stub/citro3d.h` | libctru/citro2d/citro3d host shims |
| `plat_host.c` / `plat_host.h` | synthetic hid input + citro2d draw-list recorder + render3d stubs |
| `main.c` | scripted page tour against the real `source/app.c` + `source/ui.c` + real `source/core/*.c` |
| `render_ui.py` | replay renderer + layout/legibility checker + baseline comparison |
| `Makefile` | `zig cc` build, `run`/`check` targets |

The game sources are compiled unchanged: only `<3ds.h>`, `<citro2d.h>` and
`<citro3d.h>` resolve to the shims (include order: `-Istub -I../../include
-I../../source`). No file under `source/` is patched by this harness.

## Determinism

The driver uses a fixed seed, fixed frame counts and fixture values written to
the real `GraveGame`/`App` state, so two runs produce byte-identical draw
lists. The PNG replay is deterministic for a given font/Pillow install; use
`--out out_a` / `--out out_b` and compare hashes for a determinism check.

## Extending for future UI (v0.3+)

Add a capture in `main.c` (the page fixtures live in `rep_run_state()` /
`rep_state()`), add the page name to `HUD_PAGES` / `OVERLAY_PAGES` in
`render_ui.py` if it is a paged-HUD or full-screen-overlay layout, and add the
baseline mapping when a new committed Azahar screenshot exists.

## Readability policy (v0.5 "Cute & Deadly")

Player feedback for v0.5 was that most bottom-screen writing was hard to see, so
the checker flags non-decorative text below `MIN_READABLE_SCALE` (0.335) as
`[read]`. The v0.4 tree measured **105 too-small texts across 25 pages**
(weapon micro-copy at 0.30, PAUSE at 0.28, boss name at 0.32, ...); v0.5 is
expected to drop that substantially by using fewer, bigger labels. This is
advisory: layout bounds/panels/collisions remain the blocking gate.
