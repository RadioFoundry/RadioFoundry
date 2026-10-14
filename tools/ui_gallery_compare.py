#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Compares two UI screen galleries (uiSim --gallery DIR, one PNG per screen and display class) and puts each changed
# screen side by side: before on the left, after on the right, 2x so the small displays read at a glance. Writes
# manifest.json beside the images for the pull request's comment (.github/workflows/ui-gallery*.yml, docs/dev/ci.md).
#
#   tools/ui_gallery_compare.py BEFORE AFTER OUT --base-sha SHA --head-sha SHA --pr N
#
# Output: OUT/manifest.json and OUT/compare/<geometry>/<screen>.png for every screen that is not identical. The
# contact sheets (DIR/sheets/) are left out: the comment shows screens.
import argparse
import datetime
import filecmp
import json
import os
import sys

from PIL import Image, ImageDraw

SCALE = 2
GAP = 16          # between the two screens
PAD = 12          # around the pair
HEADER = 22       # the labels above each screen
BG = (236, 236, 236)
FRAME = (120, 120, 120)
TEXT = (20, 20, 20)
MISSING = (200, 200, 200)   # a screen that isn't in one of the galleries


def screens(root):
    """(geometry, screen) for every PNG in a gallery: DIR/<geometry>/<screen>.png, never sheets/."""
    found = set()
    for geom in os.listdir(root):
        path = os.path.join(root, geom)
        if geom == "sheets" or not os.path.isdir(path):
            continue
        for name in os.listdir(path):
            if name.endswith(".png"):
                found.add((geom, name[:-4]))
    return found


def label(draw, x, y, w, text):
    draw.text((x + 4, y + 4), text, fill=TEXT)


def side_by_side(before, after, left_label, right_label, out):
    panels = []
    for path in (before, after):
        if path is None:
            panels.append(None)
        else:
            img = Image.open(path).convert("RGB")
            panels.append(img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST))
    # both sides are the same display class, so a missing screen takes the other side's size
    present = next(p for p in panels if p)
    w = [(p or present).width for p in panels]
    h = present.height
    width = PAD * 2 + w[0] + GAP + w[1]
    height = PAD * 2 + HEADER + h
    canvas = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(canvas)
    x = PAD
    for panel, text, pw in zip(panels, (left_label, right_label), w):
        label(draw, x, PAD, pw, text)
        if panel is None:
            draw.rectangle([x, PAD + HEADER, x + pw - 1, PAD + HEADER + h - 1], fill=MISSING)
            draw.text((x + 8, PAD + HEADER + 8), "not in this gallery", fill=TEXT)
        else:
            canvas.paste(panel, (x, PAD + HEADER))
            draw.rectangle([x - 1, PAD + HEADER - 1, x + pw, PAD + HEADER + panel.height], outline=FRAME)
        x += pw + GAP
    os.makedirs(os.path.dirname(out), exist_ok=True)
    canvas.save(out, optimize=True)


def main():
    ap = argparse.ArgumentParser(description="Side-by-side comparison of two UI screen galleries")
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("out")
    ap.add_argument("--base-sha", required=True)
    ap.add_argument("--head-sha", required=True)
    ap.add_argument("--pr", type=int, required=True)
    args = ap.parse_args()

    before, after = screens(args.before), screens(args.after)
    base7, head7 = args.base_sha[:7], args.head_sha[:7]
    entries = []
    counts = {"changed": 0, "added": 0, "removed": 0, "unchanged": 0}
    for geom, name in sorted(before | after):
        rel = os.path.join(geom, name + ".png")
        b = os.path.join(args.before, rel) if (geom, name) in before else None
        a = os.path.join(args.after, rel) if (geom, name) in after else None
        if b and a:
            status = "unchanged" if filecmp.cmp(b, a, shallow=False) else "changed"
        else:
            status = "added" if a else "removed"
        counts[status] += 1
        if status == "unchanged":
            continue
        image = os.path.join("compare", rel)
        side_by_side(b, a, f"before · {base7}", f"after · {head7}", os.path.join(args.out, image))
        entries.append({"geometry": geom, "screen": name, "status": status, "image": image.replace(os.sep, "/")})

    manifest = {
        "pr": args.pr,
        "base_sha": args.base_sha,
        "head_sha": args.head_sha,
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "counts": counts,
        "screens": entries,
    }
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"UI gallery: {counts['changed']} changed, {counts['added']} added, {counts['removed']} removed, "
          f"{counts['unchanged']} unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
