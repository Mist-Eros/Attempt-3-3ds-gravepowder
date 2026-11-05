"""
Gravepowder - replay renderer + layout/legibility checker (tools/preview).

Reads the citro2d draw lists that `preview.exe` dumps for every bottom-screen
page and:
  1. replays them into 320x240 PNG mockups (`out/<page>_bottom.png`) plus a
     2x nearest-neighbour view (`out/<page>_2x.png`);
  2. runs layout checks: primitive/text bounds vs 320x240, text-vs-text
     collisions (outline/shadow draws excluded), text straddling the HUD strip
     or tab-bar bands, and low-contrast text warnings (approximated citro2d
     system-font metrics);
  3. compares each mockup with the committed v0.2 Azahar baselines in
     docs/screenshots/ (the bottom screen is the 320x240 crop at (40,248) of
     the 400x488 composite) and writes amplified diff images.

The host font approximates the 3DS system font and the committed baselines are
from the v0.2 release, so text metrics and the baseline comparison are advisory
(flag-only), not golden-image tests.
"""

import argparse
import glob
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

W, H = 320, 240
# The committed 400x488 README composites: top screen 400x240 at (0,0), then a
# 320x240 bottom screen centred horizontally and placed at y=248.
BASELINE_BOTTOM_BOX = (40, 248, 40 + W, 248 + H)

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\Windows\Fonts\arial.ttf",
    r"C:\Windows\Fonts\calibri.ttf",
]

# citro2d text flags used by the game UI.
FLAG_AT_BASELINE = 1 << 0
FLAG_WITH_COLOR = 1 << 1
FLAG_ALIGN_RIGHT = 1 << 2
FLAG_ALIGN_CENTER = 2 << 2

# Paged-HUD pages (slim strip + tab bar + page body).
HUD_PAGES = {
    "hud_area", "hud_world", "hud_pack", "hud_transition", "hud_reload",
    "hud_low_hp", "hud_trail_area", "world_trail", "shop", "shop_toast",
    "hud_area_orchard", "hud_area_mine", "travel_confirm",
}
# Full-screen overlays that intentionally draw an opaque panel/dialog over the
# page underneath: overlapping text pairs and dimmed-background contrast are by
# design there.
OVERLAY_PAGES = {
    "pause", "settings", "save", "save_armed", "wardrobe", "wardrobe_reloaded",
    "title_confirm", "travel_confirm",
}
# Slide-transition frame: both pages are drawn partly off-screen on purpose.
TRANSITION_PAGES = {"hud_transition"}

# Pages whose body text lives on a big opaque panel/board/dialog.  The checker
# verifies each overlay text stays inside the panel it sits on, so longer
# control-hint lines or extra settings rows that outgrow their panel are
# flagged (the host font is an approximation; flag-only).
PANEL_PAGES = OVERLAY_PAGES | {"title", "title_save", "zone_clear", "game_over",
                               "ending"}

TAB_LABELS = {"AREA", "WORLD", "PACK", "SHOP"}
STRIP_H = 33.0     # ui.c: STRIP_H
TAB_Y = 36.0       # ui.c: TAB_Y
TAB_H = 22.0
CONTENT_Y = 62.0   # ui.c: CONTENT_Y

# Committed v0.2 Azahar screenshots that contain the matching bottom screen.
BASELINES = {
    "title": ["title.png"],
    "hud_area": ["area.png", "gameplay.png"],
    "hud_world": ["world.png"],
    "hud_pack": ["pack.png", "fire.png", "firstperson.png"],
    "pause": ["pause.png"],
}
# Committed screenshot from the older v0.1 HUD generation; kept as a note only.
LEGACY_BASELINES = {
    "weapon_switch.png": "legacy v0.1 scanner HUD layout (not the paged UI)",
}

_font_cache = {}


def load_font(em):
    key = int(em)
    if key in _font_cache:
        return _font_cache[key]
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                font = ImageFont.truetype(path, key)
                _font_cache[key] = font
                return font
            except OSError:
                continue
    font = ImageFont.load_default()
    _font_cache[key] = font
    return font


def abgr(c):
    """citro2d color u32 (ABGR) -> RGBA tuple."""
    return (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF)


def lum(rgb):
    def ch(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    return 0.2126 * ch(rgb[0]) + 0.7152 * ch(rgb[1]) + 0.0722 * ch(rgb[2])


def contrast(c1, c2):
    l1, l2 = lum(c1), lum(c2)
    if l1 < l2:
        l1, l2 = l2, l1
    return (l1 + 0.05) / (l2 + 0.05)


# ------------------------------------------------------------------ #
# draw-list parsing                                                   #
# ------------------------------------------------------------------ #

class Draw:
    __slots__ = ("kind", "nums", "clr", "flags", "text", "bbox", "idx")

    def __init__(self, kind, nums, clr=None, flags=0, text=None):
        self.kind = kind
        self.nums = nums
        self.clr = clr
        self.flags = flags
        self.text = text
        self.bbox = None  # (x0, y0, x1, y1)
        self.idx = 0      # position in the draw list (for occlusion tests)


def parse_drawlist(path):
    draws = []
    warnings = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                warnings.append(line[1:].strip())
                continue
            parts = line.split(" ")
            kind = parts[0]
            try:
                if kind == "RECT":
                    draws.append(Draw(kind, [float(v) for v in parts[1:5]],
                                      int(parts[5])))
                elif kind == "LINE":
                    draws.append(Draw(kind, [float(v) for v in parts[1:6]],
                                      int(parts[6])))
                elif kind == "TRI":
                    draws.append(Draw(kind, [float(v) for v in parts[1:7]],
                                      int(parts[7])))
                elif kind == "CIRCLE":
                    draws.append(Draw(kind, [float(v) for v in parts[1:4]],
                                      int(parts[4])))
                elif kind == "TEXT":
                    draws.append(Draw(kind, [float(parts[1]), float(parts[2]),
                                             float(parts[3])],
                                      int(parts[5]), int(parts[4]),
                                      " ".join(parts[6:])))
            except (IndexError, ValueError):
                warnings.append("malformed draw line: %s" % line[:60])
    return draws, warnings


# ------------------------------------------------------------------ #
# metrics + rendering                                                 #
# ------------------------------------------------------------------ #

def text_metrics(d, font):
    """Approximate citro2d text extents for a TEXT draw: (x0,y0,x1,y1)."""
    x, y, scale = d.nums
    text = d.text or ""
    try:
        width = float(font.getlength(text))
    except AttributeError:  # very old Pillow
        width = float(font.getsize(text)[0])
    em = font.size
    if d.flags & FLAG_ALIGN_CENTER:
        x0 = x - width / 2.0
    elif d.flags & FLAG_ALIGN_RIGHT:
        x0 = x - width
    else:
        x0 = x
    x1 = x0 + width
    # PIL 'la' anchor puts the ascender at y; the visual top sits a bit below.
    # Cap-height-ish box (0.82 em) matches the 3DS system font better than the
    # full line box and keeps panel/overlap checks honest.
    y0 = y + em * 0.10
    y1 = y + em * 0.82
    return (x0, y0, x1, y1)


def annotate(draws):
    for idx, d in enumerate(draws):
        d.idx = idx
        if d.kind == "RECT":
            x, y, w, h = d.nums
            d.bbox = (min(x, x + w), min(y, y + h), max(x, x + w), max(y, y + h))
        elif d.kind == "LINE":
            x0, y0, x1, y1, thick = d.nums
            half = max(0.5, thick * 0.5)
            d.bbox = (min(x0, x1) - half, min(y0, y1) - half,
                      max(x0, x1) + half, max(y0, y1) + half)
        elif d.kind == "TRI":
            xs, ys = d.nums[0::2], d.nums[1::2]
            d.bbox = (min(xs), min(ys), max(xs), max(ys))
        elif d.kind == "CIRCLE":
            x, y, r = d.nums
            d.bbox = (x - r, y - r, x + r, y + r)
        elif d.kind == "TEXT":
            em = max(6, int(round(33.0 * d.nums[2])))
            d.bbox = text_metrics(d, load_font(em))


def render(draws, out_path, include_text=True, save=True):
    base = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    for d in draws:
        overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        dr = ImageDraw.Draw(overlay)
        if d.kind == "RECT":
            x, y, w, h = d.nums
            if w <= 0 or h <= 0:
                continue
            dr.rectangle([x, y, x + w - 1, y + h - 1], fill=abgr(d.clr))
        elif d.kind == "LINE":
            x0, y0, x1, y1, thick = d.nums
            dr.line([x0, y0, x1, y1], fill=abgr(d.clr),
                    width=max(1, int(round(thick))))
        elif d.kind == "TRI":
            dr.polygon([(d.nums[0], d.nums[1]), (d.nums[2], d.nums[3]),
                        (d.nums[4], d.nums[5])], fill=abgr(d.clr))
        elif d.kind == "CIRCLE":
            x, y, r = d.nums
            dr.ellipse([x - r, y - r, x + r, y + r], fill=abgr(d.clr))
        elif d.kind == "TEXT" and include_text:
            x, y, scale = d.nums
            em = max(6, int(round(33.0 * scale)))
            font = load_font(em)
            if d.flags & FLAG_ALIGN_CENTER:
                anchor = "ma"
            elif d.flags & FLAG_ALIGN_RIGHT:
                anchor = "ra"
            else:
                anchor = "la"
            dr.text((x, y), d.text or "", font=font, fill=abgr(d.clr), anchor=anchor)
        base = Image.alpha_composite(base, overlay)
    rgb = base.convert("RGB")
    if save:
        rgb.save(out_path)
        rgb.resize((W * 2, H * 2), Image.NEAREST).save(out_path[:-4] + "_2x.png")
    return rgb


def covered_between(draws, first_idx, last_idx, rect, min_alpha=0.45,
                    min_coverage=0.80):
    """True when an opaque-ish RECT drawn between two primitives covers most of
    `rect` (used to ignore text pairs that are separated by a panel/toast bar)."""
    x0, y0, x1, y1 = rect
    area = max(0.0, (x1 - x0)) * max(0.0, (y1 - y0))
    if area <= 0.0:
        return True
    for k in range(first_idx + 1, last_idx):
        d = draws[k]
        if d.kind != "RECT" or d.bbox is None:
            continue
        if abgr(d.clr)[3] < int(min_alpha * 255):
            continue
        bx0, by0, bx1, by1 = d.bbox
        cx0, cy0 = max(x0, bx0), max(y0, by0)
        cx1, cy1 = min(x1, bx1), min(y1, by1)
        if cx1 <= cx0 or cy1 <= cy0:
            continue
        if (cx1 - cx0) * (cy1 - cy0) >= area * min_coverage:
            return True
    return False


# The host truetype approximation renders noticeably wider than the 3DS system
# font (the committed Azahar title screen fits "Y CAMERA SELECT TABS START
# PAUSE" inside the 240 px hint board, while the host font measures ~248 px).
# Panel-fit checks shrink the measured width by this factor so they only flag
# text that would outgrow its panel on the real hardware too.
FONT_WIDTH_CORRECTION = 0.92


def content_panels(draws):
    """Opaque, panel-sized RECTs (sign boards, dialogs, result panels).
    Full-screen clear/dim rects are excluded."""
    panels = []
    for d in draws:
        if d.kind != "RECT" or d.bbox is None:
            continue
        if abgr(d.clr)[3] < 220:
            continue
        x0, y0, x1, y1 = d.bbox
        w, h = x1 - x0, y1 - y0
        if w >= 318.0 and h >= 238.0:
            continue
        if w < 120.0 or h < 30.0:
            continue
        panels.append((d.idx, x0, y0, x1, y1))
    return panels


def overlay_content_start(page, draws):
    """Index of the last full-screen dim rect on overlay pages: text drawn after
    it is overlay content, text before it belongs to the dimmed page underneath
    (covered by the panel, so it must not be panel- or collision-checked)."""
    if page not in OVERLAY_PAGES:
        return -1
    start = -1
    for d in draws:
        if d.kind != "RECT" or d.bbox is None:
            continue
        x0, y0, x1, y1 = d.bbox
        if (x1 - x0) < 318.0 or (y1 - y0) < 238.0:
            continue
        if 60 <= abgr(d.clr)[3] <= 250:
            start = max(start, d.idx)
    return start


def panels_for_text(panels, d):
    """Panels drawn before `d` whose area contains the text centre."""
    cx = (d.bbox[0] + d.bbox[2]) * 0.5
    cy = (d.bbox[1] + d.bbox[3]) * 0.5
    return [p for p in panels
            if p[0] < d.idx and p[1] <= cx <= p[3] and p[2] <= cy <= p[4]]


def corrected_bbox(d):
    """Text bbox with the host-font width correction applied about its centre."""
    x0, y0, x1, y1 = d.bbox
    cx = (x0 + x1) * 0.5
    half = (x1 - x0) * 0.5 * FONT_WIDTH_CORRECTION
    return (cx - half, y0, cx + half, y1)


# ------------------------------------------------------------------ #
# layout / legibility checks                                          #
# ------------------------------------------------------------------ #

def check_page(page, draws, bg_image, warnings):
    issues = []
    contrast_warnings = []
    tight = []
    is_hud = page in HUD_PAGES
    skip_collision = page in OVERLAY_PAGES or page in TRANSITION_PAGES
    skip_contrast = page in OVERLAY_PAGES or page in TRANSITION_PAGES
    skip_bounds = page in TRANSITION_PAGES
    panels = content_panels(draws) if page in PANEL_PAGES else []
    tolerance = 0.75  # px tolerance for edge-hugging decorations

    def out_of_bounds(bbox):
        x0, y0, x1, y1 = bbox
        return (x0 < -tolerance or y0 < -tolerance or
                x1 > W + tolerance or y1 > H + tolerance)

    if not skip_bounds:
        for d in draws:
            if d.bbox is not None and out_of_bounds(d.bbox):
                issues.append("out of bounds: %s bbox=(%.1f,%.1f,%.1f,%.1f)%s"
                              % (d.kind, d.bbox[0], d.bbox[1], d.bbox[2],
                                 d.bbox[3],
                                 (" '" + d.text[:24] + "'") if d.text else ""))

    texts = [d for d in draws if d.kind == "TEXT" and d.text]
    content_start = overlay_content_start(page, draws)
    is_content = {d.idx: d.idx > content_start for d in texts}

    # Overlay/board pages: every overlay text that sits on a panel must stay
    # inside at least one panel containing its centre (nested border/fill rects
    # of the same board are all candidates).
    if panels:
        for d in texts:
            if not is_content[d.idx]:
                continue
            cands = [p for p in panels_for_text(panels, d)
                     if p[0] > content_start]
            if not cands:
                continue
            cb = corrected_bbox(d)
            # Small captions are deliberately drawn right at a panel edge (e.g.
            # the save-page footer sits just below the panel); allow a
            # descender-sized margin, but not a whole row/word of overflow.
            em = max(6, int(round(33.0 * d.nums[2])))
            tol = max(2.5, em * 0.55)

            def fits(p):
                return (cb[0] >= p[1] - tol and cb[1] >= p[2] - tol and
                        cb[2] <= p[3] + tol and cb[3] <= p[4] + tol)

            if not any(fits(p) for p in cands):
                p = max(cands, key=lambda q: (q[3] - q[1]) * (q[4] - q[2]))
                issues.append(
                    "text overflows its panel: '%s' bbox=(%.1f,%.1f,%.1f,%.1f) "
                    "panel=(%.0f,%.0f,%.0f,%.0f)"
                    % (d.text[:28], d.bbox[0], d.bbox[1], d.bbox[2], d.bbox[3],
                       p[1], p[2], p[3], p[4]))
            else:
                pw = (cb[2] - cb[0])
                best = max((p for p in cands if fits(p)),
                           key=lambda q: (q[3] - q[1]))
                panel_w = best[3] - best[1]
                ratio = pw / panel_w if panel_w > 0.0 else 0.0
                if ratio >= 0.95:
                    tight.append(
                        "text uses %.0f%% of its panel width: '%s' "
                        "(%.0f/%.0f px)"
                        % (ratio * 100.0, d.text[:28], pw, panel_w))

    # Overlay/board pages: a later button/panel must not cut through an earlier
    # text.  (Full coverage is intentional layering; partial coverage means a
    # control was positioned on top of a text line - flag it.)
    if page in PANEL_PAGES:
        opaque = [d for d in draws
                  if d.kind == "RECT" and d.bbox is not None and
                  abgr(d.clr)[3] >= 180 and
                  (d.bbox[2] - d.bbox[0]) < 318.0 and
                  (d.bbox[3] - d.bbox[1]) < 238.0]
        for d in texts:
            if not is_content[d.idx]:
                continue
            bx0, by0, bx1, by1 = d.bbox
            barea = max(1.0, (bx1 - bx0) * (by1 - by0))
            for r in opaque:
                if r.idx <= d.idx:
                    continue
                rx0, ry0, rx1, ry1 = r.bbox
                ix0, iy0 = max(bx0, rx0), max(by0, ry0)
                ix1, iy1 = min(bx1, rx1), min(by1, ry1)
                if ix1 <= ix0 or iy1 <= iy0:
                    continue
                covered = (ix1 - ix0) * (iy1 - iy0) / barea
                if 0.15 <= covered <= 0.95:
                    issues.append(
                        "text partly hidden under a later control/panel: "
                        "'%s' (%.0f%% covered by rect at %.0f,%.0f)"
                        % (d.text[:28], covered * 100.0, rx0, ry0))
                    break

    if is_hud:
        for d in texts:
            x0, y0, x1, y1 = d.bbox
            is_tab_label = d.text in TAB_LABELS and TAB_Y <= y0 < TAB_Y + TAB_H
            is_strip_text = y0 < STRIP_H
            if is_tab_label or is_strip_text:
                continue
            if y0 < TAB_Y + TAB_H and y1 > STRIP_H:
                issues.append("text crosses HUD strip/tab bands: '%s' (y %.1f..%.1f)"
                              % (d.text[:28], y0, y1))
            elif y0 < CONTENT_Y and y1 > STRIP_H:
                issues.append("text crosses tab bar/content boundary: '%s' (y %.1f..%.1f)"
                              % (d.text[:28], y0, y1))
            if y1 > H - 2.0:
                issues.append("text below the safe area: '%s' bottom=%.1f"
                              % (d.text[:28], y1))

    if not skip_collision or panels:
        for i in range(len(texts)):
            a = texts[i]
            for j in range(i + 1, len(texts)):
                b = texts[j]
                # On overlay pages only overlay content is compared; text that
                # belongs to the dimmed page underneath is covered by the panel
                # and may legitimately overlap it.
                if content_start >= 0 and not (is_content[a.idx] and
                                               is_content[b.idx]):
                    continue
                ax0, ay0, ax1, ay1 = a.bbox
                bx0, by0, bx1, by1 = b.bbox
                ix0, iy0 = max(ax0, bx0), max(ay0, by0)
                ix1, iy1 = min(ax1, bx1), min(ay1, by1)
                if ix1 <= ix0 or iy1 <= iy0:
                    continue
                em = max(6, int(round(33.0 * max(a.nums[2], b.nums[2]))))
                shadow_tol = max(5.0, em * 0.35)
                if (a.text == b.text and
                        abs(a.nums[0] - b.nums[0]) <= shadow_tol and
                        abs(a.nums[1] - b.nums[1]) <= shadow_tol):
                    continue  # deliberate outline/shadow copy
                # A panel/toast bar drawn between the two texts hides the
                # earlier one, so the overlap is intentional layering.
                if covered_between(draws, a.idx, b.idx, (ix0, iy0, ix1, iy1)):
                    continue
                area = (ix1 - ix0) * (iy1 - iy0)
                min_area = min((ax1 - ax0) * (ay1 - ay0),
                               (bx1 - bx0) * (by1 - by0))
                if min_area > 0 and area / min_area > 0.25:
                    issues.append("text collision: '%s' overlaps '%s'"
                                  % (a.text[:22], b.text[:22]))

    if not skip_contrast:
        # Estimate the surface the text sits on from the background-only render
        # (no glyphs): take the dominant colour inside the text's occupied box,
        # so decorative rules/particles under the box do not skew the sample.
        # Outline/shadow copies (same string, <= shadow_tol offset, followed by
        # a later copy) are skipped so only the core text is judged.
        for i, d in enumerate(texts):
            color = abgr(d.clr)
            if color[3] <= 40 or lum(color[:3]) <= 0.03:
                continue
            em = max(6, int(round(33.0 * d.nums[2])))
            shadow_tol = max(5.0, em * 0.35)
            copies = [other for other in texts
                      if other.text == d.text and
                      abs(other.nums[0] - d.nums[0]) <= shadow_tol and
                      abs(other.nums[1] - d.nums[1]) <= shadow_tol]
            # Big engraved headings are a stylised multi-copy outline style;
            # their fill colour is intentionally close to the paper.
            if len(copies) >= 3:
                continue
            is_under_copy = any(other.idx > d.idx for other in copies)
            if is_under_copy:
                continue
            bx0, by0, bx1, by1 = d.bbox
            bx0 = int(min(max(bx0, 0), W - 1))
            by0 = int(min(max(by0, 0), H - 1))
            bx1 = int(min(max(bx1, bx0 + 1), W))
            by1 = int(min(max(by1, by0 + 1), H))
            counts = {}
            for yy in range(by0, by1, 2):
                for xx in range(bx0, bx1, 2):
                    px = bg_image.getpixel((xx, yy))
                    key = (px[0] >> 4, px[1] >> 4, px[2] >> 4)
                    entry = counts.get(key)
                    if entry is None:
                        counts[key] = [1, px[0], px[1], px[2]]
                    else:
                        entry[0] += 1
                        entry[1] += px[0]
                        entry[2] += px[1]
                        entry[3] += px[2]
            if not counts:
                continue
            n, sr, sg, sb = max(counts.values(), key=lambda e: e[0])
            bg = (sr / n, sg / n, sb / n)
            cr = contrast(color[:3], bg)
            if cr < 2.2:
                contrast_warnings.append(
                    "low contrast %.1f:1: '%s' at (%.0f,%.0f)"
                    % (cr, d.text[:28], d.nums[0], d.nums[1]))

    for w in warnings:
        issues.append("drawlist: " + w)

    # De-duplicate while keeping order.
    seen = set()
    unique = []
    for issue in issues:
        if issue not in seen:
            seen.add(issue)
            unique.append(issue)
    seen_tight = set()
    unique_tight = []
    for t in tight:
        if t not in seen_tight:
            seen_tight.add(t)
            unique_tight.append(t)
    return unique, contrast_warnings, unique_tight


def image_mae(diff):
    mean = ImageStat.Stat(diff).mean
    return sum(mean) / 3.0


def compare_baseline(page, mock_rgb, baselines_dir, outdir):
    results = []
    for name in BASELINES.get(page, []):
        path = os.path.join(baselines_dir, name)
        if not os.path.exists(path):
            results.append((name, None, None, "missing"))
            continue
        try:
            ref = Image.open(path).convert("RGB").crop(BASELINE_BOTTOM_BOX)
        except Exception as exc:  # corrupt/odd baseline
            results.append((name, None, None, "error: %s" % exc))
            continue
        diff = ImageChops.difference(mock_rgb, ref)
        mae = image_mae(diff)
        # 20x15 block comparison: mean channel delta > 30 = mismatch.
        small = diff.resize((20, 15), Image.BOX)
        data = small.tobytes()
        bad = total = 0
        for i in range(0, len(data), 3):
            total += 1
            if (data[i] + data[i + 1] + data[i + 2]) / 3.0 > 30.0:
                bad += 1
        diff_path = os.path.join(outdir, "diff_%s_%s" % (page, name))
        diff.point(lambda v: min(255, v * 3)).save(diff_path)
        results.append((name, mae, (bad, total), diff_path))
    return results


# ------------------------------------------------------------------ #
# main                                                                #
# ------------------------------------------------------------------ #

def main():
    ap = argparse.ArgumentParser(description="Gravepowder UI preview replay + checks")
    ap.add_argument("--out", default=os.path.join(HERE, "out"),
                    help="draw-list/output directory (default: tools/preview/out)")
    ap.add_argument("--baselines",
                    default=os.path.join(REPO, "docs", "screenshots"),
                    help="committed Azahar screenshot directory")
    ap.add_argument("--no-compare", action="store_true",
                    help="skip the baseline image comparison")
    args = ap.parse_args()

    outdir = os.path.abspath(args.out)
    baselines = os.path.abspath(args.baselines)
    os.makedirs(outdir, exist_ok=True)

    drawlists = sorted(glob.glob(os.path.join(outdir, "*.txt")))
    drawlists = [p for p in drawlists
                 if not os.path.basename(p).startswith(("check", "diff"))]
    if not drawlists:
        print("render_ui: no draw lists in %s - run preview.exe first" % outdir)
        return 1

    def rel(path):
        return os.path.relpath(path, REPO) if path.startswith(REPO) else path

    report = []
    total_issues = 0
    total_contrast = 0
    total_tight = 0
    total_compares = 0

    print("Gravepowder UI preview replay (%d page draw list(s))" % len(drawlists))
    for path in drawlists:
        page = os.path.splitext(os.path.basename(path))[0]
        draws, warnings = parse_drawlist(path)
        annotate(draws)
        png_path = os.path.join(outdir, page + "_bottom.png")
        rendered = render(draws, png_path, include_text=True, save=True)
        bg_image = render(draws, png_path, include_text=False, save=False)
        issues, contrast_warnings, tight = check_page(page, draws, bg_image, warnings)
        total_issues += len(issues)
        total_contrast += len(contrast_warnings)
        total_tight += len(tight)

        n_text = sum(1 for d in draws if d.kind == "TEXT")
        print("\n[page] %-18s %3d primitives  %2d text  -> %s"
              % (page, len(draws), n_text, rel(png_path)))
        if issues:
            for issue in issues:
                print("  [layout] " + issue)
        else:
            print("  [layout] OK (bounds, band boundaries, panels, collisions)")
        if tight:
            print("  [tight] %d near-panel-width text(s)" % len(tight))
            for t in tight[:6]:
                print("    " + t)
            if len(tight) > 6:
                print("    ... %d more" % (len(tight) - 6))
        if contrast_warnings:
            print("  [contrast] %d warning(s)" % len(contrast_warnings))
            for cw in contrast_warnings[:6]:
                print("    " + cw)
            if len(contrast_warnings) > 6:
                print("    ... %d more" % (len(contrast_warnings) - 6))
        report.append((page, issues, contrast_warnings, tight))

        if not args.no_compare:
            for result in compare_baseline(page, rendered, baselines, outdir):
                total_compares += 1
                if result[1] is None:
                    if result[3] == "missing":
                        print("  [compare] no baseline %s" % result[0])
                    else:
                        print("  [compare] %s -> %s" % (result[0], result[3]))
                else:
                    name, mae, bad, diff_path = result
                    verdict = "close" if mae < 15 else (
                        "moderate" if mae < 35 else "DIFFERS")
                    print("  [compare] %-18s MAE=%5.1f  block_bad=%3d/%d  %s  -> %s"
                          % (name, mae, bad[0], bad[1], verdict, rel(diff_path)))

    if not args.no_compare:
        for name, note in LEGACY_BASELINES.items():
            print("\n[compare] %s skipped: %s" % (name, note))

    print("\nrender_ui: %d page(s), %d layout issue(s), %d contrast warning(s), "
          "%d tight text(s), %d baseline comparison(s)"
          % (len(report), total_issues, total_contrast, total_tight,
             total_compares))
    print("          mockups: %s/<page>_bottom.png (+ _2x.png; diff_*.png for compares)"
          % rel(outdir))

    report_path = os.path.join(outdir, "check_report.txt")
    with open(report_path, "w", encoding="utf-8") as f:
        for page, issues, cws, tight in report:
            f.write("[page] %s\n" % page)
            for issue in issues:
                f.write("  [layout] %s\n" % issue)
            for t in tight:
                f.write("  [tight] %s\n" % t)
            for cw in cws:
                f.write("  [contrast] %s\n" % cw)
        f.write("\nlayout issues: %d, contrast warnings: %d, tight texts: %d\n"
                % (total_issues, total_contrast, total_tight))
    print("          report:  %s" % rel(report_path))
    return 0 if total_issues == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
