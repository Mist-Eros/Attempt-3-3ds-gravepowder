#!/usr/bin/env python3
"""
GPM v1 self-test / inspection tool.

    python tools/gpm_selftest.py data/models/hero.gpm
    python tools/gpm_selftest.py data/models/*.gpm --bones
    python tools/gpm_selftest.py data/models/zombie.gpm --clips
    python tools/gpm_selftest.py data/models/gun_musket.gpm --preview gun.png

Parses every file, runs the same strict structural validation the importer
runs after writing, prints a stats table and exits non-zero if any file is
invalid. This is the reference reader for the format in docs/assets.md; the
importer (tools/import_model.py) is the reference writer.

Options:
  --bones              print the exported bone table (index, parent, rest TRS)
  --clips              print every animation clip with its tracks/key counts
  --preview PNG        render the model to a PNG preview (last file only)
  --preview-clip ROLE  pose that preview with this clip (default: rest pose)
  --preview-time SEC   clip time for the preview (default 0.0)
  --quiet              only print problems / the final summary
  --json               print the stats as JSON instead of the text block
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from import_model import (  # noqa: E402  (same directory, reference reader)
    COMPONENT_ROTATION,
    COMPONENT_SCALE,
    COMPONENT_TRANSLATION,
    gpm_stats,
    read_gpm,
    render_preview,
    validate_gpm,
)

COMPONENT_NAMES = {
    COMPONENT_TRANSLATION: "T",
    COMPONENT_ROTATION: "R",
    COMPONENT_SCALE: "S",
}

KNOWN_ROLES = ["idle", "walk", "run", "shoot", "reload", "attack", "hit",
               "death"]


def stats_dict(gpm):
    return {
        "path": gpm.path,
        "bytes": gpm.file_size,
        "flags": gpm.flags,
        "skinned": gpm.skinned,
        "vertex_count": gpm.vertex_count,
        "index_count": gpm.index_count,
        "triangle_count": gpm.triangle_count,
        "bone_count": gpm.bone_count,
        "anim_count": gpm.anim_count,
        "bbox_min": [float(v) for v in gpm.bbox_min],
        "bbox_max": [float(v) for v in gpm.bbox_max],
        "clips": [{"name": clip.name, "duration": clip.duration,
                   "tracks": len(clip.tracks),
                   "keys": sum(len(track.times) for track in clip.tracks)}
                  for clip in gpm.clips],
        "credit": gpm.credit,
    }


def print_bones(gpm):
    if not gpm.bones:
        print("  bones: none (static model)")
        return
    print(f"  bones ({gpm.bone_count}, parent-before-child):")
    for index, bone in enumerate(gpm.bones):
        rotation = "[" + ", ".join(f"{v:+.4f}" for v in bone.rotation) + "]"
        translation = "[" + ", ".join(f"{v:+.4f}"
                                      for v in bone.translation) + "]"
        scale = "[" + ", ".join(f"{v:.4f}" for v in bone.scale) + "]"
        print(f"    [{index:2d}] parent {bone.parent:3d}  {bone.name:28s} "
              f"T{translation} R{rotation} S{scale}")
    # attachment-name hints for the renderer
    hints = {
        "head": ["Head", "mixamorig:Head", "head"],
        "hips": ["Hips", "mixamorig:Hips", "hip"],
        "right hand/wrist": ["Wrist.R", "Hand.R", "mixamorig:RightHand"],
    }
    print("  attachment hints:")
    for role, candidates in hints.items():
        found = None
        for candidate in candidates:
            for index, bone in enumerate(gpm.bones):
                if bone.name == candidate:
                    found = (index, bone.name)
                    break
            if found:
                break
        print(f"    {role:18s} -> "
              + (f"#{found[0]} {found[1]}" if found else "NOT FOUND"))


def print_clips(gpm):
    if not gpm.clips:
        print("  clips: none (static model)")
        return
    print(f"  clips ({gpm.anim_count}):")
    for clip in gpm.clips:
        keys = sum(len(track.times) for track in clip.tracks)
        components = ""
        if clip.tracks:
            counts = {COMPONENT_NAMES[c]: 0 for c in COMPONENT_NAMES}
            for track in clip.tracks:
                counts[COMPONENT_NAMES[track.component]] += 1
            components = "  tracks " + "/".join(
                f"{name}{counts[name]}" for name in ("T", "R", "S"))
        role = "standard" if clip.name in KNOWN_ROLES else "NON-STANDARD NAME"
        print(f"    {clip.name:8s} {clip.duration:6.2f}s  "
              f"{len(clip.tracks):3d} tracks / {keys:4d} keys  ({role})"
              f"{components}")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate and inspect GPM v1 model files.")
    parser.add_argument("files", nargs="+", help="*.gpm file(s)")
    parser.add_argument("--bones", action="store_true",
                        help="print the bone table")
    parser.add_argument("--clips", action="store_true",
                        help="print animation clip details")
    parser.add_argument("--preview", default=None,
                        help="render the last file to a PNG preview")
    parser.add_argument("--preview-clip", default=None,
                        help="pose the preview with this clip")
    parser.add_argument("--preview-time", type=float, default=0.0,
                        help="preview clip time in seconds")
    parser.add_argument("--quiet", action="store_true",
                        help="only print problems and the final summary")
    parser.add_argument("--json", action="store_true",
                        help="print stats as JSON")
    args = parser.parse_args(argv)

    failures = 0
    summaries = []
    last_gpm = None
    for path in args.files:
        if not os.path.isfile(path):
            print(f"error: {path}: file not found")
            failures += 1
            continue
        try:
            gpm = read_gpm(path)
        except Exception as exc:  # noqa: BLE001 - report any malformed file
            print(f"FAIL {path}: {exc}")
            failures += 1
            continue
        problems = validate_gpm(gpm, verbose=not args.quiet)
        summaries.append(stats_dict(gpm))
        last_gpm = gpm
        if not args.quiet:
            print(f"PASS {path}")
            print(gpm_stats(gpm))
            if args.bones:
                print_bones(gpm)
            if args.clips:
                print_clips(gpm)
            print("")
        if problems:
            for problem in problems:
                print(f"error: {path}: {problem}")
            failures += 1

    if args.json:
        print(json.dumps(summaries, indent=2))

    total_bytes = sum(item["bytes"] for item in summaries)
    total_verts = sum(item["vertex_count"] for item in summaries)
    print(f"{len(summaries)} file(s) checked, {total_bytes:,} bytes total "
          f"({total_bytes / 1024.0 / 1024.0:.2f} MB), {total_verts:,} vertices")
    if failures:
        print(f"RESULT: FAIL ({failures} file(s) with errors)")
        return 1
    print("RESULT: PASS")
    if args.preview and last_gpm is not None:
        render_preview(last_gpm, args.preview, clip_name=args.preview_clip,
                       time_seconds=args.preview_time)
        print(f"preview written: {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
