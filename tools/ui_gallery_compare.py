#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Compares two UI screen galleries (uiSim --gallery DIR, one PNG per screen and display class) and makes the pictures
# a pull request's comment shows: for each display class that has a changed screen, one or more composites, each a
# stack of the changed screens as before | after pairs, 2x so the small displays read at a glance (1x when a row would
# be wider than MAX_WIDTH). A class with no change gets no picture. Writes manifest.json beside them
# (.github/workflows/ui-gallery*.yml, docs/dev/ci.md).
#
#   tools/ui_gallery_compare.py BEFORE AFTER OUT --base-sha SHA --head-sha SHA --pr N [--colors "red yellow"]
#   tools/ui_gallery_compare.py --self-test     the grouping rules (tools/ci.sh; needs no Pillow)
#
# Output: OUT/manifest.json and OUT/compare/<geometry>-<n>.png, at most ROWS_PER_IMAGE rows each. The manifest lists
# every screen that is not identical, with its status, and which composite it is in; each composite's entry has its
# row count (the comment folds a tall one).
#
# A color page's highlight color and theme shots (<screen>__<color>, uiSim UI_GALLERY_COLORS; <screen>__light and
# __high_contrast) stay together: when any of them or the page itself changed, the whole set is shown in one row: a
# column per shot, the page first, then its colors in --colors order, then its themes, before above after (unchanged
# ones marked so). Such a row counts as two toward ROWS_PER_IMAGE.
import argparse
import datetime
import filecmp
import json
import os
import sys

SCALE = 2
THEMES = ("light", "high_contrast")    # uiSim's theme shots, after the colors
GAP = 24          # between the two screens of a pair, room for the arrow
PAD = 12          # around the picture
ROW_HEADER = 20   # the screen's name above each pair
LABEL = 20        # before / after, above each screen
ROWS_PER_IMAGE = 8
MAX_WIDTH = 1800  # wider than this at 2x, a composite is drawn at 1x
BG = (236, 236, 236)
FRAME = (120, 120, 120)
TEXT = (20, 20, 20)
MISSING = (200, 200, 200)   # a screen that isn't in one of the galleries
ARROW = (90, 90, 90)        # before -> after


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


def page(name):
    """(screen, color) of a gallery name: home_strip__red -> (home_strip, red); a plain screen has color ""."""
    base, _, color = name.partition("__")
    return base, color


def page_groups(rows, colors):
    """rows grouped by page, each group the page first, then its colors in the configured order, then its themes."""
    order = {c: i for i, c in enumerate(list(colors) + list(THEMES))}
    groups = {}
    for row in rows:
        groups.setdefault(page(row[0])[0], []).append(row)
    for g in groups.values():
        g.sort(key=lambda r: (page(r[0])[1] != "", order.get(page(r[0])[1], len(order)), page(r[0])[1]))
    return [groups[k] for k in sorted(groups)]


def weight(g):
    """A composite row's share of ROWS_PER_IMAGE: a color page's grid is two screens high."""
    return 1 if len(g) == 1 else 2


def chunks(groups):
    """Composites of at most ROWS_PER_IMAGE rows (a group is one row, never split); each with a changed screen."""
    parts, part = [], []
    for g in groups:
        if part and sum(map(weight, part)) + weight(g) > ROWS_PER_IMAGE:
            parts.append(part)
            part = []
        part.append(g)
    if part:
        parts.append(part)
    return [p for p in parts if any(r[1] != "unchanged" for g in p for r in g)]


def self_test():
    rows = [("home_strip__yellow", "changed", None, None), ("home_strip__high_contrast", "changed", None, None),
            ("home_strip", "unchanged", None, None), ("home_strip__light", "unchanged", None, None),
            ("home_strip__red", "unchanged", None, None), ("home", "changed", None, None),
            ("home_strip_monitor", "changed", None, None)]
    assert page("home_strip__light_blue") == ("home_strip", "light_blue") and page("home") == ("home", "")
    groups = page_groups(rows, ["red", "yellow"])
    assert [[r[0] for r in g] for g in groups] == [["home"], ["home_strip", "home_strip__red", "home_strip__yellow",
                                                    "home_strip__light", "home_strip__high_contrast"],
                                                   ["home_strip_monitor"]], groups
    one = [[("p%d" % i, "changed", None, None)] for i in range(7)]
    color = [("c", "unchanged", None, None)] + [("c__c%d" % i, "changed" if i else "unchanged", None, None)
                                                 for i in range(3)]
    parts = chunks(one + [color])
    assert [[len(g) for g in p] for p in parts] == [[1] * 7, [4]], parts   # 7 + 2 > 8: the grid starts the next one
    assert chunks([[("d", "unchanged", None, None)]]) == []                # nothing changed, no composite
    print("ui_gallery_compare self-test passed")
    return 0


def scaled(path, scale):
    from PIL import Image
    img = Image.open(path).convert("RGB")
    return img.resize((img.width * scale, img.height * scale), Image.NEAREST)


def is_mono(paths):
    """True when every picture of a display class has at most two colors (a 1-bit panel)."""
    from PIL import Image
    for p in paths:
        if Image.open(p).convert("RGB").getcolors(maxcolors=3) is None:
            return False
    return True


def class_scale(groups):
    """SCALE, or 1 when the class's widest row would be wider than MAX_WIDTH; one scale for all its composites."""
    from PIL import Image
    sw = Image.open(next(p for g in groups for r in g for p in r[2:] if p)).width
    cols = max(max(len(g), 2) for g in groups)
    return SCALE if 2 * PAD + cols * (sw * SCALE + GAP) - GAP <= MAX_WIDTH else 1


def composite(groups, scale, before_label, after_label, out):
    """groups: [[(name, status, before_path or None, after_path or None)]], one row each, stacked into one PNG. A lone
    screen is a before | after pair; a color page is a grid, a column per shot, before above after."""
    from PIL import Image, ImageDraw
    sw, sh = Image.open(next(p for g in groups for r in g for p in r[2:] if p)).size
    cols = max(max(len(g), 2) for g in groups)
    w, h = sw * scale, sh * scale
    height = PAD + LABEL
    for g in groups:
        height += ROW_HEADER + (LABEL + h + PAD if len(g) == 1 else LABEL + 2 * (LABEL + h + PAD))
    canvas = Image.new("RGB", (2 * PAD + cols * (w + GAP) - GAP, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, PAD // 2 + 2), f"{before_label}  |  {after_label}", fill=TEXT)

    def arrow(cx, cy, down):
        """A small arrow from before to after, centered on (cx, cy): right between a pair, down in a grid column."""
        L, H = 10, 6  # half length, half head width
        if down:
            draw.rectangle([cx - 1, cy - L, cx + 1, cy + L - H], fill=ARROW)
            draw.polygon([(cx - H, cy + L - H), (cx + H, cy + L - H), (cx, cy + L)], fill=ARROW)
        else:
            draw.rectangle([cx - L, cy - 1, cx + L - H, cy + 1], fill=ARROW)
            draw.polygon([(cx + L - H, cy - H), (cx + L - H, cy + H), (cx + L, cy)], fill=ARROW)

    def shot(path, x, top):
        if path:
            canvas.paste(scaled(path, scale), (x, top))
            draw.rectangle([x - 1, top - 1, x + w, top + h], outline=FRAME)
        else:
            draw.rectangle([x, top, x + w - 1, top + h - 1], fill=MISSING)
            draw.text((x + 8, top + 8), "not in this gallery", fill=TEXT)

    y = PAD + LABEL
    for g in groups:
        if len(g) == 1:
            name, status, b, a = g[0]
            draw.text((PAD, y), f"{name} ({status})", fill=TEXT)
            y += ROW_HEADER
            for i, (path, label) in enumerate(((b, before_label), (a, after_label))):
                x = PAD + i * (w + GAP)
                draw.text((x + 2, y), label, fill=TEXT)
                shot(path, x, y + LABEL)
            arrow(PAD + w + GAP // 2, y + LABEL + h // 2, False)
            y += LABEL + h + PAD
            continue
        draw.text((PAD, y), page(g[0][0])[0], fill=TEXT)
        y += ROW_HEADER
        for i, (name, status, _, _) in enumerate(g):
            draw.text((PAD + i * (w + GAP) + 2, y), f"{page(name)[1] or 'default'} ({status})", fill=TEXT)
        y += LABEL
        for k, label in ((2, before_label), (3, after_label)):
            draw.text((PAD + 2, y), label, fill=TEXT)
            for i, row in enumerate(g):
                shot(row[k], PAD + i * (w + GAP), y + LABEL)
                if k == 2:   # in the gap down to the after row, clear of its label at the left
                    arrow(PAD + i * (w + GAP) + w // 2, y + LABEL + h + (PAD + LABEL) // 2, True)
            y += LABEL + h + PAD
    os.makedirs(os.path.dirname(out), exist_ok=True)
    canvas.save(out, optimize=True)


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description="Composite comparison of two UI screen galleries")
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("out")
    ap.add_argument("--base-sha", required=True)
    ap.add_argument("--head-sha", required=True)
    ap.add_argument("--pr", type=int, required=True)
    ap.add_argument("--colors", default="", help="the gallery's highlight colors, in order (UI_GALLERY_COLORS)")
    args = ap.parse_args()
    colors = args.colors.replace(",", " ").split()

    before, after = screens(args.before), screens(args.after)
    base7, head7 = args.base_sha[:7], args.head_sha[:7]
    counts = {"changed": 0, "added": 0, "removed": 0, "unchanged": 0}
    rows = {}      # geometry -> [(screen, status, before path, after path)], every screen
    for geom, name in sorted(before | after):
        rel = os.path.join(geom, name + ".png")
        b = os.path.join(args.before, rel) if (geom, name) in before else None
        a = os.path.join(args.after, rel) if (geom, name) in after else None
        if b and a:
            status = "unchanged" if filecmp.cmp(b, a, shallow=False) else "changed"
        else:
            status = "added" if a else "removed"
        counts[status] += 1
        rows.setdefault(geom, []).append((name, status, b, a))
    changed = {}   # geometry -> its groups with a change (a color page's unchanged shots kept beside the changed)
    for geom, geom_rows in rows.items():
        groups = [g for g in page_groups(geom_rows, colors) if any(r[1] != "unchanged" for r in g)]
        if groups:
            changed[geom] = groups

    images = []   # manifest entries: one per composite
    screens_out = []
    # monochrome classes first, then color; within each, by resolution
    def size(g):
        w, h = g.split("x")[0], g.split("x")[1]
        return int("".join(c for c in w if c.isdigit())), int("".join(c for c in h if c.isdigit()))
    kind = {g: "mono" if is_mono([p for grp in changed[g] for _, _, b, a in grp for p in (b, a) if p]) else "color"
            for g in changed}
    for geom in sorted(changed, key=lambda g: (kind[g] != "mono", size(g), g)):
        scale = class_scale(changed[geom])
        for n, part in enumerate(chunks(changed[geom]), 1):
            image = os.path.join("compare", f"{geom}-{n}.png")
            composite(part, scale, f"before · {base7}", f"after · {head7}", os.path.join(args.out, image))
            image = image.replace(os.sep, "/")
            shown = [r for g in part for r in g if r[1] != "unchanged"]
            images.append({"geometry": geom, "color": kind[geom], "image": image, "rows": len(part),
                           "screens": [r[0] for r in shown]})
            for name, status, _, _ in shown:
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
