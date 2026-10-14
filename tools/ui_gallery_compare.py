#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Compares two UI screen galleries (uiSim --gallery DIR, one PNG per screen and display class) and makes the pictures
# a pull request's comment shows: for each display class that has a changed screen, one or more composites, each a
# stack of the changed screens as before | after pairs, 2x so the small displays read at a glance. A class with no
# change gets no picture. Writes manifest.json beside them (.github/workflows/ui-gallery*.yml, docs/dev/ci.md).
#
#   tools/ui_gallery_compare.py BEFORE AFTER OUT --base-sha SHA --head-sha SHA --pr N
#
# Output: OUT/manifest.json and OUT/compare/<geometry>-<n>.png, at most ROWS_PER_IMAGE screens each. The manifest lists
# every screen that is not identical, with its status, and which composite it is in.
import argparse
import datetime
import filecmp
import json
import os
import sys

from PIL import Image, ImageDraw

SCALE = 2
GAP = 16          # between the two screens of a pair
PAD = 12          # around the picture
ROW_HEADER = 20   # the screen's name above each pair
LABEL = 20        # before / after, above each screen
ROWS_PER_IMAGE = 8
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


def scaled(path):
    img = Image.open(path).convert("RGB")
    return img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)


def is_mono(paths):
    """True when every picture of a display class has at most two colors (a 1-bit panel)."""
    for p in paths:
        if Image.open(p).convert("RGB").getcolors(maxcolors=3) is None:
            return False
    return True


def composite(rows, before_label, after_label, out):
    """rows: [(name, status, before_path or None, after_path or None)], stacked top to bottom into one PNG."""
    pairs = []
    for name, status, b, a in rows:
        bi = scaled(b) if b else None
        ai = scaled(a) if a else None
        present = bi or ai
        pairs.append((name, status, bi or present, ai or present, bi is not None, ai is not None))
    w = max(2 * PAD + p[2].width + GAP + p[3].width for p in pairs)
    height = PAD + LABEL + sum(ROW_HEADER + LABEL + p[2].height + PAD for p in pairs)
    canvas = Image.new("RGB", (w, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, PAD // 2 + 2), f"{before_label}  |  {after_label}", fill=TEXT)
    y = PAD + LABEL
    for name, status, bi, ai, has_b, has_a in pairs:
        draw.text((PAD, y), f"{name} ({status})", fill=TEXT)
        y += ROW_HEADER
        x = PAD
        for img, has, label in ((bi, has_b, before_label), (ai, has_a, after_label)):
            draw.text((x + 2, y), label, fill=TEXT)
            top = y + LABEL
            if has:
                canvas.paste(img, (x, top))
                draw.rectangle([x - 1, top - 1, x + img.width, top + img.height], outline=FRAME)
            else:
                draw.rectangle([x, top, x + img.width - 1, top + img.height - 1], fill=MISSING)
                draw.text((x + 8, top + 8), "not in this gallery", fill=TEXT)
            x += img.width + GAP
        y += LABEL + bi.height + PAD
    os.makedirs(os.path.dirname(out), exist_ok=True)
    canvas.save(out, optimize=True)


def main():
    ap = argparse.ArgumentParser(description="Composite comparison of two UI screen galleries")
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("out")
    ap.add_argument("--base-sha", required=True)
    ap.add_argument("--head-sha", required=True)
    ap.add_argument("--pr", type=int, required=True)
    args = ap.parse_args()

    before, after = screens(args.before), screens(args.after)
    base7, head7 = args.base_sha[:7], args.head_sha[:7]
    counts = {"changed": 0, "added": 0, "removed": 0, "unchanged": 0}
    changed = {}   # geometry -> [(screen, status, before path, after path)]
    for geom, name in sorted(before | after):
        rel = os.path.join(geom, name + ".png")
        b = os.path.join(args.before, rel) if (geom, name) in before else None
        a = os.path.join(args.after, rel) if (geom, name) in after else None
        if b and a:
            status = "unchanged" if filecmp.cmp(b, a, shallow=False) else "changed"
        else:
            status = "added" if a else "removed"
        counts[status] += 1
        if status != "unchanged":
            changed.setdefault(geom, []).append((name, status, b, a))

    images = []   # manifest entries: one per composite
    screens_out = []
    # monochrome classes first, then color; within each, by resolution
    def size(g):
        w, h = g.split("x")[0], g.split("x")[1]
        return int("".join(c for c in w if c.isdigit())), int("".join(c for c in h if c.isdigit()))
    kind = {g: "mono" if is_mono([p for _, _, b, a in changed[g] for p in (b, a) if p]) else "color" for g in changed}
    for geom in sorted(changed, key=lambda g: (kind[g] != "mono", size(g), g)):
        rows = changed[geom]
        for n, start in enumerate(range(0, len(rows), ROWS_PER_IMAGE), 1):
            part = rows[start:start + ROWS_PER_IMAGE]
            image = os.path.join("compare", f"{geom}-{n}.png")
            composite(part, f"before · {base7}", f"after · {head7}", os.path.join(args.out, image))
            image = image.replace(os.sep, "/")
            images.append({"geometry": geom, "color": kind[geom], "image": image, "screens": [name for name, *_ in part]})
            for name, status, _, _ in part:
                screens_out.append({"geometry": geom, "screen": name, "status": status, "image": image})

    manifest = {
        "pr": args.pr,
        "base_sha": args.base_sha,
        "head_sha": args.head_sha,
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "counts": counts,
        "images": images,
        "screens": screens_out,
    }
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"UI gallery: {counts['changed']} changed, {counts['added']} added, {counts['removed']} removed, "
          f"{counts['unchanged']} unchanged, {len(images)} image(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
