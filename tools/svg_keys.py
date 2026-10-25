#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Check and add the key tags of the radio drawings (docs/dev/website.md "Radio images").

    tools/svg_keys.py check [BOARD...]          every board with a drawing when none is named
    tools/svg_keys.py tag BOARD [--from FILE] [--dry-run]
    tools/svg_keys.py --self-test

The web control page (CONFIG_NET_WEBCTL) makes each element with id="key-<name>" in site/static/img/radios/src/
BOARD.svg (and BOARD-mic.svg, its hand microphone) press that key. check fails when a key id names no key of the
board, is used twice, has a bad data-edge, or when a key of the board (its KEY_* enum, tools/gen_webctl_assets.py) is
in neither drawing and not listed in the drawing's data-undrawn="name ..." on its <svg> element. The site build runs
the same check (site/rfoundrysite/device.py), so a redrawn radio that lost its tags fails CI.

tag adds the missing tags to BOARD.svg in two passes, and prints each one for review:
  1. by label: a shape that holds a <text> naming one key ("MENU", "VFO/MR" for vfo_mr, "*" for star, "#" for
     hash) and no other key's label gets that key's id (the smallest such shape);
  2. by the last tagged version (--from FILE, or the newest revision in git with key ids): its tagged shapes are
     mapped onto the new drawing by a fit of the keys both versions share, and each goes to the nearest untagged
     shape of about its size, a shape of the same fill first. data-edge, led and encoder come along, and hit rects
     (class="hits") are moved with the shape they lie on.
What is left is printed: tag it by hand, or list it in data-undrawn. Open the drawing on the web control page (or
render it) to confirm the result before committing.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "site", "static", "img", "radios", "src")
EDGES = ("left", "right", "top", "bottom")
SHAPE = re.compile(r"<(rect|circle|ellipse|path|polygon)\b([^>]*?)/?>", re.S)
TEXT = re.compile(r"<text\b([^>]*)>(.*?)</text>", re.S)
ID = re.compile(r"\bid=\"([^\"]*)\"")
TAG_ID = re.compile(r"\bid=\"(key-[^\"]*|led|encoder)\"")
KEY_ID = re.compile(r"\bid=\"key-([^\"]*)\"")
EDGE = re.compile(r"\bdata-edge=\"([^\"]*)\"")
UNDRAWN = re.compile(r"<svg\b[^>]*\bdata-undrawn=\"([^\"]*)\"")


def key_names(board):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_webctl_assets
    return sorted(gen_webctl_assets.key_bits(gen_webctl_assets.key_header(board)))


def problems(text, keys):
    """What is wrong with the tags of a drawing (text: the radio's and its microphone's drawings together), given
    the board's key names. An empty list when the drawing has no key ids at all: the page then shows buttons."""
    ids = KEY_ID.findall(text)
    if not ids:
        return []
    out = []
    bad = sorted(set(i for i in ids if i not in keys))
    if bad:
        out.append("key ids the radio has no key for: %s (its keys: %s)" % (", ".join(bad), ", ".join(keys)))
    twice = sorted(set(i for i in ids if ids.count(i) > 1))
    if twice:
        out.append("key ids used twice: %s" % ", ".join(twice))
    edges = sorted(set(EDGE.findall(text)) - set(EDGES))
    if edges:
        out.append("data-edge must be left, right, top or bottom, not %s" % ", ".join(edges))
    undrawn = " ".join(UNDRAWN.findall(text)).split()
    if sorted(set(undrawn) - set(keys)):
        out.append("data-undrawn names no key of the radio: %s" % ", ".join(sorted(set(undrawn) - set(keys))))
    if sorted(set(undrawn) & set(ids)):
        out.append("data-undrawn lists tagged keys: %s" % ", ".join(sorted(set(undrawn) & set(ids))))
    missing = [k for k in keys if k not in ids and k not in undrawn]
    if missing:
        out.append("keys in no drawing: %s (tag them: tools/svg_keys.py tag, or list them in the <svg> element's "
                   "data-undrawn)" % ", ".join(missing))
    return out


def drawings(board):
    svg = os.path.join(SRC, board + ".svg")
    mic = os.path.join(SRC, board + "-mic.svg")
    return svg, (mic if os.path.isfile(mic) else None)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def check(boards):
    if not boards:
        boards = sorted(b for b in os.listdir(os.path.join(ROOT, "boards"))
                        if os.path.isfile(os.path.join(SRC, b + ".svg")))
    fails = 0
    for b in boards:
        svg, mic = drawings(b)
        text = read(svg) + (read(mic) if mic else "")
        for p in problems(text, key_names(b)):
            print("%s: %s" % (os.path.relpath(svg, ROOT), p))
            fails += 1
    if not fails:
        print("svg_keys: %d drawing%s, every key tagged or listed undrawn" % (len(boards), "" if len(boards) == 1 else "s"))
    return 1 if fails else 0


# Geometry: a bounding box per shape, from its attributes or its path data (control points included, close enough
# to match shapes)

def num(attrs, name, default=None):
    m = re.search(r"(?:^|\s)%s=\"([-\d.e]+)\"" % name, attrs)
    return float(m.group(1)) if m else default


def path_box(d):
    toks = re.findall(r"[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", d)
    nargs = {"M": 2, "L": 2, "T": 2, "H": 1, "V": 1, "Q": 4, "S": 4, "C": 6, "A": 7, "Z": 0}
    pts, x, y, sx, sy, cmd, i = [], 0.0, 0.0, 0.0, 0.0, "M", 0
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                x, y = sx, sy
                continue
        n = nargs[cmd.upper()]
        a = [float(t) for t in toks[i:i + n]]
        i += n
        if len(a) < n:
            break
        rel = cmd.islower()
        u = cmd.upper()
        if u == "H":
            x = a[0] + (x if rel else 0)
        elif u == "V":
            y = a[0] + (y if rel else 0)
        elif u == "A":
            x, y = a[5] + (x if rel else 0), a[6] + (y if rel else 0)
        else:
            for k in range(0, n, 2):
                px, py = a[k] + (x if rel else 0), a[k + 1] + (y if rel else 0)
                pts.append((px, py))
            x, y = pts[-1]
        pts.append((x, y))
        if u == "M":
            sx, sy = x, y
            cmd = "l" if rel else "L"
    if not pts:
        return None
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def box(kind, attrs):
    if kind == "rect":
        x, y, w, h = num(attrs, "x", 0), num(attrs, "y", 0), num(attrs, "width"), num(attrs, "height")
        return None if w is None or h is None else (x, y, x + w, y + h)
    if kind in ("circle", "ellipse"):
        cx, cy = num(attrs, "cx", 0), num(attrs, "cy", 0)
        rx = num(attrs, "r") if kind == "circle" else num(attrs, "rx")
        ry = num(attrs, "r") if kind == "circle" else num(attrs, "ry")
        return None if rx is None or ry is None else (cx - rx, cy - ry, cx + rx, cy + ry)
    m = re.search(r"\b(?:d|points)=\"([^\"]*)\"", attrs)
    if not m:
        return None
    if kind == "polygon":
        v = [float(t) for t in re.findall(r"[-+]?(?:\d*\.\d+|\d+\.?)", m.group(1))]
        return path_box("M" + " ".join("%g %g" % (v[k], v[k + 1]) for k in range(0, len(v) - 1, 2))) if v else None
    return path_box(m.group(1))


def spans(text, pattern):
    return [(m.start(), m.end()) for m in re.finditer(pattern, text, re.S)]


def parse(text):
    """Shapes (dict: kind, start, attrs, box, id, fill, hit) and labels (x, y, text) outside <defs>; shapes with a
    transform of their own are left out (their box would be wrong)."""
    skip = spans(text, r"<defs\b.*?</defs>")
    hits = spans(text, r"<g\b[^>]*class=\"hits\"[^>]*>.*?</g>")
    inside = lambda pos, sp: any(a <= pos < b for a, b in sp)
    shapes = []
    for m in SHAPE.finditer(text):
        if inside(m.start(), skip) or re.search(r"\btransform=", m.group(2)):
            continue
        b = box(m.group(1), m.group(2))
        if b is None:
            continue
        i = ID.search(m.group(2))
        f = re.search(r"\bfill=\"([^\"]*)\"", m.group(2))
        shapes.append({"kind": m.group(1), "start": m.start(), "attrs": m.group(2), "box": b,
                       "id": i.group(1) if i else None, "fill": f.group(1) if f else None,
                       "edge": (EDGE.search(m.group(2)) or [None, None])[1], "hit": inside(m.start(), hits)})
    labels = []
    for m in TEXT.finditer(text):
        x, y = num(m.group(1), "x"), num(m.group(1), "y")
        if x is not None and y is not None:
            labels.append((x, y, re.sub(r"<[^>]*>", "", m.group(2)).strip()))
    return shapes, labels


def label_key(s, keys):
    s = s.replace("&amp;", "&").strip().lower()
    s = {"*": "star", "#": "hash"}.get(s, s)
    for c in (s, re.sub(r"[/\s.-]+", "_", s), re.sub(r"[/\s._-]+", "", s)):
        if c in keys:
            return c
    return None


def tagid(name):
    return name if name in ("led", "encoder") else "key-" + name


def name_of(i):
    return i[4:] if i.startswith("key-") else i


def center(b):
    return ((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)


def size(b):
    return (b[2] - b[0], b[3] - b[1])


def area(b):
    return (b[2] - b[0]) * (b[3] - b[1])


def contains(b, x, y):
    return b[0] <= x <= b[2] and b[1] <= y <= b[3]


def fit(pairs, old_span, new_span):
    """new = a * old + b on one axis, least squares over the matched centers; the drawings' spans when fewer than
    two distinct points."""
    xs = [p[0] for p in pairs]
    if len(set(round(v, 1) for v in xs)) < 2:
        a = new_span[1] / old_span[1] if old_span[1] else 1.0
        return a, new_span[0] - a * old_span[0]
    n = len(pairs)
    mx, my = sum(xs) / n, sum(p[1] for p in pairs) / n
    a = sum((p[0] - mx) * (p[1] - my) for p in pairs) / sum((v - mx) ** 2 for v in xs)
    return a, my - a * mx


def viewbox(text):
    m = re.search(r"viewBox=\"([-\d.\s]+)\"", text)
    v = [float(t) for t in m.group(1).split()] if m else [0, 0, 1, 1]
    return v


def match(old, new_free, mapx, mapy, used):
    """The untagged new shape that an old shape maps to: about its size, near where it maps, the same fill first."""
    (ax, bx), (ay, by) = mapx, mapy
    cx, cy = center(old["box"])
    mx, my = ax * cx + bx, ay * cy + by
    w, h = size(old["box"])
    mw, mh = abs(ax) * w, abs(ay) * h
    diag = max((mw * mw + mh * mh) ** 0.5, 1.0)
    best = None
    for s in new_free:
        if id(s) in used:
            continue
        sw, sh = size(s["box"])
        if not (0.5 * mw <= sw <= 2 * mw + 1 and 0.5 * mh <= sh <= 2 * mh + 1):
            continue
        nx, ny = center(s["box"])
        cost = ((nx - mx) ** 2 + (ny - my) ** 2) ** 0.5 / diag
        if s["fill"] and s["fill"] == old["fill"]:
            cost *= 0.5
        if cost < 3 and (best is None or cost < best[0]):
            best = (cost, s)
    return best


def plan(text, keys, old_text=None, other_ids=()):
    """The tags to add to a drawing: [(shape or None, name, how, hit box or None)], and the keys still missing."""
    shapes, labels = parse(text)
    have = set(name_of(s["id"]) for s in shapes if s["id"] and TAG_ID.fullmatch('id="%s"' % s["id"]))
    have |= set(other_ids)
    free = [s for s in shapes if not s["id"] and not s["hit"]]
    out, done, used = [], set(), set()
    # 1. by label
    named = [(x, y, label_key(t, keys)) for x, y, t in labels]
    named = [n for n in named if n[2]]
    picks = {}
    for x, y, k in named:
        if k in have:
            continue
        cands = [s for s in free if contains(s["box"], x, y)
                 and not any(k2 != k and contains(s["box"], x2, y2) for x2, y2, k2 in named)]
        if cands:
            s = min(cands, key=lambda s: area(s["box"]))
            picks.setdefault(k, set()).add(id(s))
            out.append((s, k, "label", None))
    # a shape claimed by two keys, or a key on two shapes, is not settled by its label
    claims = {}
    for s, k, _, _ in out:
        claims.setdefault(id(s), set()).add(k)
    out = [o for o in out if len(claims[id(o[0])]) == 1 and len(picks[o[1]]) == 1]
    seen = set()
    out = [o for o in out if not (o[1] in seen or seen.add(o[1]))]
    for s, k, _, _ in out:
        done.add(k)
        used.add(id(s))
    # 2. by the last tagged version
    if old_text:
        olds, _ = parse(old_text)
        tagged = {name_of(s["id"]): s for s in olds if s["id"] and not s["hit"]
                  and TAG_ID.fullmatch('id="%s"' % s["id"])}
        now = {name_of(s["id"]): s for s in shapes if s["id"] and not s["hit"]}
        now.update({k: s for s, k, _, _ in out})
        # a key found by its label keeps the edge it had
        out = [(s, k, how + (", data-edge %s" % tagged[k]["edge"] if k in tagged and tagged[k]["edge"] else ""),
                tagged[k]["edge"] if k in tagged and tagged[k]["edge"] else None) for s, k, how, _ in out]
        pairs = [(center(tagged[k]["box"]), center(now[k]["box"])) for k in tagged if k in now]
        ov, nv = viewbox(old_text), viewbox(text)
        mapx = fit([(o[0], n[0]) for o, n in pairs], (ov[0], ov[2]), (nv[0], nv[2]))
        mapy = fit([(o[1], n[1]) for o, n in pairs], (ov[1], ov[3]), (nv[1], nv[3]))
        cand = []
        for k, o in tagged.items():
            if k in have or k in done or (k not in keys and k not in ("led", "encoder")):
                continue
            best = match(o, free, mapx, mapy, set())
            if best:
                cand.append((best[0], k, o, best[1]))
        for cost, k, o, s in sorted(cand, key=lambda c: c[0]):
            if k in done or id(s) in used:
                continue
            out.append((s, k, "last tagged version", None))
            if o["edge"]:
                out[-1] = (s, k, "last tagged version, data-edge %s" % o["edge"], o["edge"])
            done.add(k)
            used.add(id(s))
        # hit rects move with the smallest old shape under them, matched like a key
        for o in olds:
            if not (o["hit"] and o["id"]):
                continue
            k = name_of(o["id"])
            if k in have or k in done or k not in keys:
                continue
            ob = o["box"]
            under = [s for s in olds if not s["hit"] and s is not o and s["box"][0] <= ob[0] and s["box"][1] <= ob[1]
                     and s["box"][2] >= ob[2] and s["box"][3] >= ob[3] and area(s["box"]) > 0]
            if under:
                u = min(under, key=lambda s: area(s["box"]))
                best = match(u, [s for s in shapes if not s["hit"]], mapx, mapy, set())
                if best:
                    ub, nb = u["box"], best[1]["box"]
                    sx = (nb[2] - nb[0]) / ((ub[2] - ub[0]) or 1)
                    sy = (nb[3] - nb[1]) / ((ub[3] - ub[1]) or 1)
                    hb = (nb[0] + (ob[0] - ub[0]) * sx, nb[1] + (ob[1] - ub[1]) * sy,
                          nb[0] + (ob[2] - ub[0]) * sx, nb[1] + (ob[3] - ub[1]) * sy)
                    out.append((None, k, "hit rect, moved with the shape under it", hb))
                    done.add(k)
                    continue
            hb = (mapx[0] * ob[0] + mapx[1], mapy[0] * ob[1] + mapy[1], mapx[0] * ob[2] + mapx[1],
                  mapy[0] * ob[3] + mapy[1])
            out.append((None, k, "hit rect, moved by the fit", hb))
            done.add(k)
    undrawn = " ".join(UNDRAWN.findall(text)).split()
    missing = [k for k in keys if k not in have and k not in done and k not in undrawn]
    return out, missing


def apply(text, tags):
    edits = []
    hits = []
    for s, k, how, extra in tags:
        if s is None:
            b = extra
            hits.append('<rect id="%s" x="%g" y="%g" width="%g" height="%g" fill="none" pointer-events="all"/>'
                        % (tagid(k), round(b[0], 1), round(b[1], 1), round(b[2] - b[0], 1), round(b[3] - b[1], 1)))
            continue
        attr = ' id="%s"' % tagid(k)
        if isinstance(extra, str) and not s["edge"]:
            attr += ' data-edge="%s"' % extra
        p = s["start"] + 1 + len(s["kind"])
        edits.append((p, attr))
    for p, a in sorted(edits, reverse=True):
        text = text[:p] + a + text[p:]
    if hits:
        m = re.search(r"(<g\b[^>]*class=\"hits\"[^>]*>.*?)(</g>)", text, re.S)
        if m:
            text = text[:m.end(1)] + "".join(hits) + text[m.end(1):]
        else:
            i = text.rindex("</svg>")
            text = text[:i] + '<g class="hits">' + "".join(hits) + "</g>\n" + text[i:]
    return text


def last_tagged(path):
    rel = os.path.relpath(path, ROOT)
    try:
        revs = subprocess.run(["git", "-C", ROOT, "log", "--format=%H", "--", rel], capture_output=True, text=True,
                              check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return None, None
    for r in revs:
        t = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (r, rel)], capture_output=True, text=True).stdout
        if KEY_ID.search(t):
            return t, r[:8]
    return None, None


def tag(board, src=None, dry=False):
    svg, mic = drawings(board)
    if not os.path.isfile(svg):
        raise SystemExit("svg_keys: %s has no drawing (%s)" % (board, os.path.relpath(svg, ROOT)))
    text = read(svg)
    if src:
        old, origin = read(src), src
    else:
        old, origin = last_tagged(svg)
    other = [name_of(i) for i in TAG_ID.findall(read(mic))] if mic else []
    tags, missing = plan(text, key_names(board), old, other)
    for s, k, how, extra in tags:
        b = s["box"] if s else extra
        print("  %-10s %-44s at %g,%g" % (tagid(k), how, round(b[0], 1), round(b[1], 1)))
    if old is None:
        print("  (no tagged version in git to carry tags from; --from FILE names one)")
    else:
        print("  (tags carried from %s)" % origin)
    if missing:
        print("not placed: %s. Tag them by hand, or list them in the <svg> element's data-undrawn"
              % ", ".join(missing))
    if tags and not dry:
        with open(svg, "w", encoding="utf-8") as f:
            f.write(apply(text, tags))
        print("svg_keys: %d tag%s added to %s; check it on the web control page"
              % (len(tags), "" if len(tags) == 1 else "s", os.path.relpath(svg, ROOT)))
    return 1 if missing else 0


def self_test():
    fails = []

    def ok(cond, what):
        if not cond:
            fails.append(what)
    keys = ["1", "2", "menu", "vfo_mr", "ptt", "sk1", "up", "down", "star"]
    head = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 400">'
    old = (head + '<rect x="10" y="100" width="10" height="60" fill="#111"/>'
           '<rect id="key-ptt" data-edge="left" x="0" y="100" width="10" height="60" fill="url(#k)"/>'
           '<rect id="key-sk1" data-edge="left" x="0" y="180" width="10" height="30" fill="url(#k)"/>'
           '<rect id="led" x="90" y="10" width="20" height="6" fill="#888"/>'
           '<rect x="80" y="200" width="40" height="40" fill="url(#pad)"/>'
           '<rect id="key-1" x="40" y="300" width="30" height="20"/><rect id="key-2" x="120" y="300" width="30" '
           'height="20"/><rect id="key-menu" x="40" y="260" width="30" height="20"/>'
           '<g class="hits"><rect id="key-up" x="80" y="200" width="40" height="20" fill="none" pointer-events="all"/>'
           '<rect id="key-down" x="80" y="220" width="40" height="20" fill="none" pointer-events="all"/></g></svg>')
    # the redraw: everything 10 lower and 5 to the right, the side keys moved, no ids
    new = (head.replace('<svg ', '<svg data-undrawn="vfo_mr" ') + '<defs><rect x="0" y="0" width="5" height="5"/></defs>'
           '<path d="M5 100 L15 100 L15 170 L5 170Z" fill="#111"/>'
           '<rect x="2" y="112" width="12" height="60" fill="url(#k)"/>'
           '<text x="8" y="140" transform="rotate(-90 8 140)">PTT</text>'
           '<rect x="2" y="195" width="12" height="28" fill="url(#k)"/>'
           '<rect x="95" y="20" width="20" height="6" fill="#888"/>'
           '<rect x="85" y="210" width="40" height="40" fill="url(#pad)"/>'
           '<rect x="45" y="310" width="30" height="20"/><text x="55" y="325">1</text>'
           '<rect x="125" y="310" width="30" height="20"/><text x="135" y="325">2</text><text x="145" y="325">ABC</text>'
           '<rect x="45" y="270" width="30" height="20"/><rect x="47" y="271" width="26" height="6"/>'
           '<text x="60" y="285">MENU</text>'
           '<rect x="125" y="270" width="30" height="20"/><text x="130" y="285">*</text></svg>')
    tags, missing = plan(new, keys, old)
    got = {k: (how, s["box"] if s else extra) for s, k, how, extra in tags}
    ok(got.get("1", ("", (0,)))[1][:2] == (45, 310), "label 1 on its key, not on a text-free shape: %r" % (got.get("1"),))
    ok(got.get("menu", ("",))[0] == "label" and got["menu"][1][:2] == (45, 270), "MENU on the key, not its highlight")
    ok(got.get("star", ("",))[0] == "label", "* is star")
    ok(got.get("ptt", ("",))[0] == "label, data-edge left", "PTT by its rotated label, its edge kept: %r"
       % (got.get("ptt"),))
    ok(got.get("sk1", ("", (0, 0)))[1][:2] == (2, 195) and "left" in got["sk1"][0], "sk1 carried with its edge: %r"
       % (got.get("sk1"),))
    ok(got.get("led", ("", (0, 0)))[1][:2] == (95, 20), "led carried by its fill: %r" % (got.get("led"),))
    up = got.get("up", ("", (0, 0, 0, 0)))[1]
    ok(tuple(round(v) for v in up) == (85, 210, 125, 230), "up hit rect moved with the pad: %r" % (up,))
    ok(missing == [], "nothing missing (vfo_mr is undrawn): %r" % missing)
    out = apply(new, tags)
    ok('<rect id="key-sk1" data-edge="left" x="2" y="195"' in out, "sk1 written with its edge")
    ok(out.count('class="hits"') == 1 and 'id="key-down"' in out, "hit rects written in a hits group")
    ok('<rect x="0" y="0" width="5"' in out, "<defs> left alone")
    ok(problems(out, keys) == [], "the tagged drawing passes: %r" % problems(out, keys))
    p = problems(new + '<rect id="key-1"/><rect id="key-1"/><rect id="key-zz" data-edge="up"/>', keys)
    ok(any("no key for: zz" in s for s in p), "unknown key")
    ok(any("twice: 1" in s for s in p), "duplicate")
    ok(any("not up" in s for s in p), "bad edge")
    ok(any("keys in no drawing:" in s and "menu" in s and "vfo_mr" not in s.split("(")[0] for s in p),
       "missing keys, the undrawn one left out: %r" % p)
    ok(problems(head + "</svg>", keys) == [], "a drawing without key ids is fine (the page shows buttons)")
    ok(label_key("VFO/MR", keys) == "vfo_mr" and label_key("A/B", ["ab"]) == "ab", "label names")
    ok(path_box("M246 645l8-12l8 12z") == (246, 633, 262, 645), "relative path box: %r" % (path_box("M246 645l8-12l8 12z"),))
    print("svg_keys self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                     "" if len(fails) == 1 else "s"))
    for f in fails:
        print("  " + f)
    return 1 if fails else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv and argv[0] == "check":
        return check(argv[1:])
    if len(argv) >= 2 and argv[0] == "tag":
        src = argv[argv.index("--from") + 1] if "--from" in argv else None
        return tag(argv[1], src, "--dry-run" in argv)
    print(__doc__.strip())
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
