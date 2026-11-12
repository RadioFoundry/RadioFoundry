# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Codec maps: where each supported stock build keeps its AMBE+2 codec, and which words in it hold addresses.

docs/spec/codec-placement.md ("Codec maps"). One JSON file per stock build in codec_maps/<board>/<profile>.json,
format "rfoundry-codec-map/1". A map holds addresses, lengths, offsets and hashes, never codec bytes. The merge tools use it
to move the codec as one block: a word that held an address inside the old block gets the same offset in the new one.

Used by rfoundry, tools/hrc7000_codec.py and (exported through site/rfoundrysite/repo.py) the web flasher's codecmap.js,
which is a port of this file: the two must move a codec to the same bytes (tools/rfoundry/tests and site/tests share the
vectors in codec_maps/test_vectors.json).
"""
import hashlib
import json
import os
import struct
import zlib

FORMAT = "rfoundry-codec-map/1"
ISAS = ("thumb2", "csky")
KINDS = ("word", "movw_movt")
MAPS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "codec_maps")


class MapError(Exception):
    pass


def _num(v, what):
    """A map number: an integer, or a "0x..." string (the analyzer writes addresses that way for reading)."""
    if isinstance(v, bool):
        raise MapError("%s: not a number" % what)
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        try:
            return int(v, 0)
        except ValueError:
            pass
    raise MapError("%s: not a number (%r)" % (what, v))


def _site(r, what):
    """A ref's site, numbers made ints: "off" (and "off2" for a movw/movt pair) in the code, or "at" (an address)"""
    x = dict(r)
    for k in ("off", "off2", "at", "value", "target"):
        if k in x:
            x[k] = _num(x[k], "%s %s" % (what, k))
    x["kind"] = x.get("kind", "word")
    if x["kind"] not in KINDS:
        raise MapError("%s: unknown kind %r" % (what, x["kind"]))
    if x["kind"] == "movw_movt" and "off2" not in x:
        raise MapError("%s: a movw/movt pair without off2" % what)
    return x


def normalize(raw):
    """A map as loaded from JSON, with every address and length as an int. Raises MapError on a malformed map."""
    if raw.get("format") != FORMAT:
        raise MapError("not an %s map (format %r)" % (FORMAT, raw.get("format")))
    m = dict(raw)
    for k in ("profile", "board", "isa", "code", "entries"):
        if k not in m:
            raise MapError("map without %r" % k)
    p = m["profile"]
    if m["isa"] not in ISAS:
        raise MapError("%s: unknown isa %r" % (p, m["isa"]))
    c = dict(m["code"])
    for k in ("flash", "len", "run"):
        c[k] = _num(c.get(k), "%s code.%s" % (p, k))
    m["code"] = c
    m["entries"] = {k: _num(v, "%s entry %s" % (p, k)) for k, v in m["entries"].items()}
    for k in ("self_refs", "island_ptrs", "table_refs", "island_refs"):
        m[k] = [_site(r, "%s %s" % (p, k[:-1])) for r in m.get(k, [])]
    m["branches"] = [_site(dict(b, kind="word"), "%s branch" % p) for b in m.get("branches", [])]
    m["islands"] = [dict(i, start=_num(i["start"], "island start"), len=_num(i["len"], "island len"))
                    for i in m.get("islands", [])]
    m["objects"] = [dict(o, start=_num(o["start"], "object start"), len=_num(o["len"], "object len"))
                    for o in m.get("objects", [])]
    m["groups"] = [[_num(x, "group member") for x in g] for g in m.get("groups", [])]
    m["table_windows"] = [dict(w, **{k: _num(w[k], "%s table window %s" % (p, k))
                                     for k in ("flash", "run", "len", "min_start", "min_len") if k in w})
                          for w in m.get("table_windows", [])]
    m["outside_refs"] = list(m.get("outside_refs", []))
    check(m)
    return m


def _ref_sites(r):
    return [r["off"], r["off2"]] if r["kind"] == "movw_movt" else [r["off"]]


def check(m):
    """The checks every map must pass before it is used: refs inside the code (whole aligned words, or movw/movt
    instruction pairs), entries inside the code, table refs pointing into a listed object, branches from an island
    into the code, nothing pointing outside the codec."""
    p, c = m["profile"], m["code"]
    if c["len"] <= 0:
        raise MapError("%s: empty code window" % p)
    for name, off in m["entries"].items():
        if not 0 <= off < c["len"]:
            raise MapError("%s: entry %s +0x%X outside the code" % (p, name, off))

    def site_ok(r, what):
        if r["kind"] == "word":
            if r["off"] % 4 or not 0 <= r["off"] <= c["len"] - 4:
                raise MapError("%s: %s at +0x%X is not an aligned word inside the code" % (p, what, r["off"]))
        else:
            if m["isa"] != "thumb2":
                raise MapError("%s: %s at +0x%X: movw/movt pairs are Thumb-2 only" % (p, what, r["off"]))
            for o in _ref_sites(r):
                if o % 2 or not 0 <= o <= c["len"] - 4:
                    raise MapError("%s: %s at +0x%X is not an instruction inside the code" % (p, what, o))

    for r in m["self_refs"]:
        site_ok(r, "self ref")
        if not c["run"] <= r["value"] < c["run"] + c["len"]:
            raise MapError("%s: self ref at +0x%X (0x%08X) points outside the code" % (p, r["off"], r["value"]))
    for r in m["island_ptrs"]:
        site_ok(r, "island pointer")
        if island_of(m, r["target"]) is None:
            raise MapError("%s: island pointer at +0x%X (0x%08X) points into no island" % (p, r["off"], r["target"]))
    objs = m["objects"]
    for r in m["table_refs"]:
        site_ok(r, "table ref")
        if objs and object_of(m, r["target"]) is None:
            raise MapError("%s: table ref at +0x%X (0x%08X) points into no object" % (p, r["off"], r["target"]))
    for r in m["island_refs"] + m["branches"]:
        if "at" not in r or island_of(m, r["at"]) is None:
            raise MapError("%s: island ref or branch at 0x%08X lies in no island" % (p, r.get("at", 0)))
    for r in m["island_refs"]:
        if r["at"] % 4 or (objs and object_of(m, r["target"]) is None):
            raise MapError("%s: island ref at 0x%08X is not an aligned word into an object" % (p, r["at"]))
    for b in m["branches"]:
        if m["isa"] != "thumb2" or b["at"] % 2 or not c["run"] <= b["target"] < c["run"] + c["len"]:
            raise MapError("%s: branch at 0x%08X is not a Thumb-2 bl into the code" % (p, b["at"]))
    if m["outside_refs"]:
        raise MapError("%s: the codec refers outside its windows; it can't be moved" % p)
    offs = [o for k in ("self_refs", "island_ptrs", "table_refs") for r in m[k] for o in _ref_sites(r)]
    ats = [r["at"] for r in m["island_refs"] + m["branches"]]
    if len(offs) != len(set(offs)) or len(ats) != len(set(ats)):
        raise MapError("%s: a site is listed twice" % p)


def island_of(m, addr):
    """The island an address lies in (its start), or None"""
    for i in m["islands"]:
        if i["start"] <= addr < i["start"] + i["len"]:
            return i["start"]
    return None


def object_of(m, addr):
    """The object an address points into (its start), or None"""
    for o in m["objects"]:
        if o["start"] <= addr < o["start"] + max(o["len"], 1):
            return o["start"]
    return None


def load_maps(directory=MAPS_DIR):
    """Every map under codec_maps/, by profile id"""
    out = {}
    if not os.path.isdir(directory):
        return out
    for board in sorted(os.listdir(directory)):
        d = os.path.join(directory, board)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".json"):
                continue
            with open(os.path.join(d, name)) as f:
                m = normalize(json.load(f))
            if m["profile"] in out:
                raise MapError("two maps for profile %s" % m["profile"])
            if m["board"] != board:
                raise MapError("%s: map for %s filed under %s" % (name, m["board"], board))
            out[m["profile"]] = m
    return out


def by_code_sha(maps, sha):
    """The maps whose carved code has this SHA-256"""
    return [m for m in maps.values() if m["code"].get("sha256") == sha]


def _word(b, off):
    return struct.unpack_from("<I", b, off)[0]


def _half(b, off):
    return struct.unpack_from("<H", b, off)[0]


# Thumb-2 MOVW and MOVT (encoding T3): imm16 = imm4:i:imm3:imm8 across the two halfwords
_MOVW, _MOVT = 0xF240, 0xF2C0


def _imm16(b, off, op):
    h1, h2 = _half(b, off), _half(b, off + 2)
    if h1 & 0xFBF0 != op or h2 & 0x8000:
        return None
    return (h1 & 0xF) << 12 | (h1 >> 10 & 1) << 11 | (h2 >> 12 & 7) << 8 | (h2 & 0xFF)


def _put_imm16(b, off, v):
    h1, h2 = _half(b, off), _half(b, off + 2)
    h1 = (h1 & 0xFBF0) | (v >> 12 & 0xF) | (v >> 11 & 1) << 10
    h2 = (h2 & 0x8F00) | (v >> 8 & 7) << 12 | (v & 0xFF)
    struct.pack_into("<HH", b, off, h1, h2)


def pair_value(b, r):
    """The address a movw/movt pair builds, or None when the two instructions aren't there"""
    lo, hi = _imm16(b, r["off"], _MOVW), _imm16(b, r["off2"], _MOVT)
    return None if lo is None or hi is None else hi << 16 | lo


def _put_pair(b, r, v):
    _put_imm16(b, r["off"], v & 0xFFFF)
    _put_imm16(b, r["off2"], v >> 16)


def bl_target(b, off, at):
    """The target of the Thumb-2 BL (encoding T1) at b[off], which sits at address at; None if it isn't one"""
    h1, h2 = _half(b, off), _half(b, off + 2)
    if h1 & 0xF800 != 0xF000 or h2 & 0xD000 != 0xD000:
        return None
    s = h1 >> 10 & 1
    i1, i2 = 1 - ((h2 >> 13 & 1) ^ s), 1 - ((h2 >> 11 & 1) ^ s)
    imm = s << 24 | i1 << 23 | i2 << 22 | (h1 & 0x3FF) << 12 | (h2 & 0x7FF) << 1
    if s:
        imm -= 1 << 25
    return at + 4 + imm


def put_bl(b, off, at, target):
    """Write a BL at b[off] (address at) to target"""
    imm = target - (at + 4)
    if imm % 2 or not -(1 << 24) <= imm < (1 << 24):
        raise MapError("bl at 0x%08X can't reach 0x%08X" % (at, target))
    imm &= 0x1FFFFFF
    s, i1, i2 = imm >> 24, imm >> 23 & 1, imm >> 22 & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    struct.pack_into("<HH", b, off, 0xF000 | s << 10 | (imm >> 12 & 0x3FF), 0xD000 | j1 << 13 | j2 << 11 | (imm >> 1 & 0x7FF))


def _read(b, r):
    return _word(b, r["off"]) if r["kind"] == "word" else pair_value(b, r)


def _write(b, r, v):
    if r["kind"] == "word":
        struct.pack_into("<I", b, r["off"], v & 0xFFFFFFFF)
    else:
        _put_pair(b, r, v & 0xFFFFFFFF)


def move_code(code, m, new_run):
    """The code bytes as they must be to run at new_run: every self ref (a word, or a movw/movt pair) rewritten by
    (new_run - stock run). The input must be the map's own code (checked by SHA-256 when the map has it) and every
    listed site must still hold the value the map says, so a wrong map or a wrong build stops here instead of producing
    a codec that jumps astray. Islands stay where they are: their pointers in the code don't change, but their branches
    into the code do (move_branches)."""
    c = m["code"]
    if len(code) != c["len"]:
        raise MapError("%s: code is %d bytes, the map says %d" % (m["profile"], len(code), c["len"]))
    if c.get("sha256") and hashlib.sha256(code).hexdigest() != c["sha256"]:
        raise MapError("%s: the code is not the build this map describes" % m["profile"])
    if (new_run - c["run"]) % c.get("move_alignment", 4):
        raise MapError("%s: a move by 0x%X breaks the code's alignment" % (m["profile"], new_run - c["run"]))
    delta = new_run - c["run"]
    out = bytearray(code)
    for r in m["self_refs"] + m["island_ptrs"]:
        got = _read(out, r)
        want = r["value"] if "value" in r else r["target"]
        if got != want:
            raise MapError("%s: site +0x%X holds %s, the map says 0x%08X"
                           % (m["profile"], r["off"], "nothing" if got is None else "0x%08X" % got, want))
    for r in m["self_refs"]:
        _write(out, r, r["value"] + delta)
    return bytes(out)


def move_branches(addr, data, m, new_run):
    """The bytes data (at address addr: a table window holding islands) with every listed BL inside them that calls
    the code re-encoded for the code at new_run. Each BL must decode to the target the map says."""
    out = bytearray(data)
    delta = new_run - m["code"]["run"]
    for b in m["branches"]:
        if not addr <= b["at"] <= addr + len(data) - 4:
            continue
        off = b["at"] - addr
        got = bl_target(out, off, b["at"])
        if got != b["target"]:
            raise MapError("%s: no bl to 0x%08X at 0x%08X" % (m["profile"], b["target"], b["at"]))
        put_bl(out, off, b["at"], b["target"] + delta)
    return bytes(out)


def branches_in(m, addr, length):
    """How many listed branches lie in [addr, addr + length)"""
    return sum(1 for b in m["branches"] if addr <= b["at"] < addr + length)


def move_tables(code, m, moves):
    """The code with its table refs rewritten for moved objects. moves: {object start: new start}; an object not in
    moves stays. Objects in one group must move by the same amount. Moving an object an island refers to, or one
    reached by a movw/movt pair, is not supported yet."""
    for g in m["groups"]:
        if len({moves.get(s, s) - s for s in g}) > 1:
            raise MapError("%s: objects of one group moved by different amounts" % m["profile"])
    moved = {o for o, n in moves.items() if n != o}
    for r in m["island_refs"]:
        if object_of(m, r["target"]) in moved:
            raise MapError("%s: object 0x%08X is referred to from an island; not supported yet" % (m["profile"], r["target"]))
    out = bytearray(code)
    for r in m["table_refs"]:
        if _read(out, r) != r["target"]:
            raise MapError("%s: site +0x%X doesn't hold 0x%08X" % (m["profile"], r["off"], r["target"]))
        o = object_of(m, r["target"])
        if o not in moved:
            continue
        if r["kind"] != "word":
            raise MapError("%s: table ref at +0x%X is a %s; not supported yet" % (m["profile"], r["off"], r["kind"]))
        _write(out, r, r["target"] + (moves[o] - o))
    return bytes(out)


def table_window(m):
    """(start, length) of the table window a whole-window move carries: the codec's own data (the map's cut,
    min_start/min_len), which every table ref, island pointer, island ref, island and branch must lie in"""
    tw = (m["table_windows"] or [None])[0]
    if not tw or "min_len" not in tw:
        raise MapError("%s: no cut table window" % m["profile"])
    lo, n = tw.get("min_start", tw["run"]), tw["min_len"]
    inside = [r["target"] for r in m["table_refs"] + m["island_ptrs"] + m["island_refs"]] + \
             [r["at"] for r in m["island_refs"]] + [b["at"] for b in m["branches"]] + \
             [x for i in m["islands"] for x in (i["start"], i["start"] + i["len"] - 1)]
    if any(not lo <= a < lo + n for a in inside):
        raise MapError("%s: the codec refers outside its table window; the window can't move as one block" % m["profile"])
    return lo, n


def move_window(code, tables, m, code_run, tables_run):
    """The codec moved as two blocks: the code to run at code_run and its table window (the cut, table_window(), as
    the stock build has it) to tables_run. In the code every self ref moves with the code, every island pointer and
    table ref with the tables; in the tables every island ref moves with the tables and every BL from an island into
    the code is re-encoded for both moves. Returns (code, tables). The code and every listed site are checked first,
    as move_code does"""
    lo, n = table_window(m)
    if len(tables) != n:
        raise MapError("%s: tables are %d bytes, the map's window %d" % (m["profile"], len(tables), n))
    if (tables_run - lo) % 4:
        raise MapError("%s: a table move by 0x%X breaks the tables' alignment" % (m["profile"], tables_run - lo))
    moved = bytearray(move_code(code, m, code_run))
    dc, dt = code_run - m["code"]["run"], tables_run - lo
    for r in m["table_refs"]:
        if _read(moved, r) != r["target"]:
            raise MapError("%s: site +0x%X doesn't hold 0x%08X" % (m["profile"], r["off"], r["target"]))
    for r in m["island_ptrs"] + m["table_refs"]:
        _write(moved, r, r["target"] + dt)
    tab = bytearray(tables)
    for r in m["island_refs"]:
        if _word(tab, r["at"] - lo) != r["target"]:
            raise MapError("%s: island ref at 0x%08X doesn't hold 0x%08X" % (m["profile"], r["at"], r["target"]))
        struct.pack_into("<I", tab, r["at"] - lo, (r["target"] + dt) & 0xFFFFFFFF)
    for b in m["branches"]:
        if bl_target(tab, b["at"] - lo, b["at"]) != b["target"]:
            raise MapError("%s: no bl to 0x%08X at 0x%08X" % (m["profile"], b["target"], b["at"]))
        put_bl(tab, b["at"] - lo, b["at"] + dt, b["target"] + dc)
    return bytes(moved), bytes(tab)


def unmove_window(flash, base, m, code_at):
    """The reverse of move_window for a copy found in a flash read (flash: bytes from address base) whose code starts
    at code_at: (code, tables, tables_at) as the stock build has them, or None when this isn't the map's build. The
    moves are read from the copy itself: the code's from its self refs, the tables' from its table refs and island
    pointers, which must all agree. The islands' calls into the code may point anywhere, as long as all of them were
    moved by one amount (a tool that moved the code but left the tables verbatim leaves them calling the old address).
    Both parts are checked by their SHA-256 (the code's, and the cut table window's)"""
    c = m["code"]
    try:
        lo, n = table_window(m)
    except MapError:
        return None
    tw = m["table_windows"][0]
    off = code_at - base
    code = bytearray(flash[off:off + c["len"]])
    if off < 0 or len(code) != c["len"] or not tw.get("min_sha256"):
        return None
    dc = code_at - c["run"]
    for r in m["self_refs"]:
        got = _read(code, r)
        if got is None or (got - r["value"]) & 0xFFFFFFFF != dc & 0xFFFFFFFF:
            return None
        _write(code, r, r["value"])
    deltas = set()
    for r in m["island_ptrs"] + m["table_refs"]:
        got = _read(code, r)
        if got is None:
            return None
        deltas.add((got - r["target"]) & 0xFFFFFFFF)
    if len(deltas) > 1:
        return None
    dt = deltas.pop() if deltas else 0	# a code that holds no address in its tables: they are where stock had them
    dt = dt - (1 << 32) if dt >= 1 << 31 else dt
    for r in m["island_ptrs"] + m["table_refs"]:
        _write(code, r, r["target"])
    if hashlib.sha256(code).hexdigest() != c["sha256"]:
        return None
    t0 = lo + dt - base
    tab = bytearray(flash[t0:t0 + n]) if t0 >= 0 else bytearray()
    if len(tab) != n:
        return None
    for r in m["island_refs"]:
        if _word(tab, r["at"] - lo) != (r["target"] + dt) & 0xFFFFFFFF:
            return None
        struct.pack_into("<I", tab, r["at"] - lo, r["target"])
    bd = set()
    for b in m["branches"]:
        got = bl_target(tab, b["at"] - lo, b["at"] + dt)
        if got is None:
            return None
        bd.add(got - b["target"])
        put_bl(tab, b["at"] - lo, b["at"], b["target"])
    if len(bd) > 1 or hashlib.sha256(tab).hexdigest() != tw["min_sha256"]:
        return None
    return bytes(code), bytes(tab), lo + dt


def relocations(m):
    """The code offsets a firmware that moves the code itself (the HR_C7000 slot, version 2) must add the delta to:
    whole words only, and no islands (both hold for every HR_C7000 build)"""
    if any(r["kind"] != "word" for r in m["self_refs"]) or m["islands"]:
        raise MapError("%s: the code can't be moved by adding to words" % m["profile"])
    return sorted(r["off"] for r in m["self_refs"])


# ---------------------------------------------------------------------------------------------------------------
# Layouts: every codec window moved as its own block (docs/spec/codec-placement.md, "Layouts")

def _window_of(wins, addr):
    """The index of the stock window [(addr, length)] an address lies in, or None"""
    for i, (a, n) in enumerate(wins):
        if a <= addr < a + n:
            return i
    return None


def _layout_sites(m, wins, src, dst):
    """Every site a layout change rewrites: (window index, offset in it, kind, value at src, value at dst). wins: the
    stock windows [(addr, length)], the code first, as the profile carves them; src, dst: the address of each window in
    the two layouts (the stock layout is [a for a, _ in wins]). kind: "word", "movw_movt" (offset of the movw, the movt
    at offset + (off2 - off)) or "bl". Raises MapError when a site or a value lies in no window: such a codec can't be
    moved window by window"""
    p = m["profile"]
    if len(src) != len(wins) or len(dst) != len(wins):
        raise MapError("%s: a layout names %d windows, the profile has %d" % (p, len(src), len(wins)))
    if wins[0][0] != m["code"]["run"] or wins[0][1] != m["code"]["len"]:
        raise MapError("%s: the first window is not the map's code" % p)
    align = m["code"].get("move_alignment", 4)
    for i, (a, _) in enumerate(wins):
        for lay in (src, dst):
            if (lay[i] - a) % (align if i == 0 else 4):
                raise MapError("%s: window %d moved by 0x%X breaks its alignment" % (p, i, lay[i] - a))

    def moved(v, lay, what):
        w = _window_of(wins, v)
        if w is None:
            raise MapError("%s: %s 0x%08X lies in no codec window" % (p, what, v))
        return (v + lay[w] - wins[w][0]) & 0xFFFFFFFF

    out = []
    for r in m["self_refs"]:
        v = r["value"]
        out.append((0, r["off"], r, moved(v, src, "self ref"), moved(v, dst, "self ref")))
    for r in m["island_ptrs"] + m["table_refs"]:
        v = r["target"]
        out.append((0, r["off"], r, moved(v, src, "table ref"), moved(v, dst, "table ref")))
    for r in m["island_refs"]:
        w = _window_of(wins, r["at"])
        if w is None or w == 0:
            raise MapError("%s: island ref at 0x%08X lies in no table window" % (p, r["at"]))
        out.append((w, r["at"] - wins[w][0], dict(kind="word"), moved(r["target"], src, "island ref"),
                    moved(r["target"], dst, "island ref")))
    for b in m["branches"]:
        w = _window_of(wins, b["at"])
        if w is None or w == 0:
            raise MapError("%s: branch at 0x%08X lies in no table window" % (p, b["at"]))
        off = b["at"] - wins[w][0]
        out.append((w, off, dict(kind="bl"), (src[w] + off, moved(b["target"], src, "branch")),
                    (dst[w] + off, moved(b["target"], dst, "branch"))))
    return out


def _get(buf, off, r):
    if r["kind"] == "word":
        return _word(buf, off)
    if r["kind"] == "bl":
        return None
    return pair_value(buf, dict(r, off=off, off2=off + r["off2"] - r["off"]))


def _put(buf, off, r, v):
    if r["kind"] == "word":
        struct.pack_into("<I", buf, off, v & 0xFFFFFFFF)
    elif r["kind"] == "bl":
        put_bl(buf, off, v[0], v[1])
    else:
        _put_pair(buf, dict(r, off=off, off2=off + r["off2"] - r["off"]), v)


def relocate(m, wins, parts, src, dst):
    """The codec's windows moved from layout src to layout dst, each window as one block: every address in the code and
    the table windows (self refs, table refs, island pointers and refs, the islands' calls into the code) rewritten for
    where its target window goes. parts: the bytes of each window as they are at src, in wins order. Every site must
    hold exactly what layout src implies, so the wrong build, a wrong layout or damaged bytes stop here. The stock layout
    is [a for a, _ in wins], so this both places a stock codec and takes a placed one back to stock. Returns the bytes of
    each window for dst."""
    if [len(d) for d in parts] != [n for _, n in wins]:
        raise MapError("%s: the windows are %s bytes, the profile says %s"
                       % (m["profile"], [len(d) for d in parts], [n for _, n in wins]))
    out = [bytearray(d) for d in parts]
    for w, off, r, have, want in _layout_sites(m, wins, src, dst):
        if r["kind"] == "bl":
            got = bl_target(out[w], off, have[0])
            if got is None or got & 0xFFFFFFFF != have[1]:
                raise MapError("%s: no bl to 0x%08X at 0x%08X" % (m["profile"], have[1], have[0]))
        elif _get(out[w], off, r) != have:
            raise MapError("%s: window %d +0x%X doesn't hold 0x%08X" % (m["profile"], w, off, have))
        _put(out[w], off, r, want)
    return [bytes(d) for d in out]


def relocate_crcs(m, wins, crcs, src, dst):
    """The CRC-32 of each window after relocate(src -> dst), from its CRC-32 at src, without the bytes. CRC-32 is affine
    over XOR for messages of one length (crc(a ^ b) = crc(a) ^ crc(b) ^ crc(zeros)), and relocate changes only the
    listed sites, from values the map and the two layouts give. So a new layout's CRCs, which the firmware pins, follow
    from a known one's and the map alone."""
    diffs = [bytearray(n) for _, n in wins]
    for w, off, r, have, want in _layout_sites(m, wins, src, dst):
        a, b = bytearray(8), bytearray(8)
        if r["kind"] == "movw_movt":
            r2 = dict(r, off=0, off2=4)		# the immediates only; the opcode bits are the same in both
            _put_pair(a, r2, have)
            _put_pair(b, r2, want)
            gap = r["off2"] - r["off"]
            for k, o in ((0, 0), (4, gap)):
                for j in range(4):
                    diffs[w][off + o + j] ^= a[k + j] ^ b[k + j]
            continue
        _put(a, 0, r, have)
        _put(b, 0, r, want)
        for j in range(4):
            diffs[w][off + j] ^= a[j] ^ b[j]
    return [(c ^ zlib.crc32(bytes(d)) ^ zlib.crc32(bytes(len(d)))) & 0xFFFFFFFF for c, d in zip(crcs, diffs)]
