# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The codec read back from a radio that runs RadioFoundry and saved on this computer, so that a later flash needs no stock
file: where an update erases part of it (the MD-380: RadioFoundry's image shares a flash sector with the codec, boards.py
keep_codec_supported), and on every other radio for when it can't be kept (a radio back on stock firmware, a flash
that stopped halfway, a radio in its bootloader). rfoundry reads every codec window over the debug link (READ_MEM,
docs/spec/debuglink.md 3.10) and checks each against the stock profile: where stock firmware keeps it, where the merge
places it now (stockfw.placed_windows: the DM-1701's code in sector 11) or where an earlier release placed it
(stockfw.forms, "former": the MD-380's codec in sectors 8-11), a placed window by its SHA-256 or by taking it back to
the stock windows' (stockfw.check_form). The copy is checked again when it is loaded, and a window that doesn't match
can't come out of this. A copy in a stock or former layout is moved to the current one before it is written
(segments). The web flasher does the same (site/static/js/flasher/codecsave.js), keeping its copy in the browser.

The copy is never deleted: it is what lets an install that stopped halfway, at any step, be run again (docs/spec/
codec-placement.md, "Moving to a new layout").

There is one copy per radio: its file name carries the radio's chip UID (debuglink.probe "uid"), and a move only ever
uses the copy saved from the radio being moved (find, strict), so a computer that serves two radios never writes one's
codec into the other. A copy saved by an earlier rfoundry, without a UID, is used only where no UID is known or for a
radio that has no copy of its own outside a move.

The saved file is the user's own codec, from their own radio: it stays on this computer (state.config_dir()).
"""
import os
import re
import struct

from . import debuglink, state, stockfw


def profiles_for(board):
    """The stock profiles whose codec this board runs."""
    return [p for p in stockfw.PROFILES.values() if p.get("board") == board.id and p.get("codec_profile") == board.codec.id]


def _forms(prof):
    """Where a profile's codec can be in a radio: [(form, [(addr, length, sha256)])], the stock windows (form None)
    first, then the current placement and each former one (stockfw.forms)."""
    return [(None, stockfw.windows(prof))] + [(f, stockfw.placed_windows(prof, f)) for f in stockfw.forms(prof)]


def _check(prof, form, parts):
    """parts [(addr, bytes)] when they are the profile's codec in this form, else None."""
    return parts if stockfw.check_form(prof, form, parts) else None


def _described(link):
    """(tag, [window addresses]) from the descriptor in the radio's flash, or None (none there, or it can't be read)"""
    try:
        d = bytes(link.read_mem(stockfw.DESC_ADDR, stockfw.DESC_LEN))
    except (debuglink.LinkError, OSError):
        return None
    if len(d) != stockfw.DESC_LEN:
        return None
    w = struct.unpack("<%dI" % (stockfw.DESC_LEN // 4), d)
    if w[0] != stockfw.DESC_MAGIC or not 0 < w[6] <= stockfw.DESC_WINDOWS:
        return None
    return w[2], [w[3]] + [w[7 + 3 * i] for i in range(w[6])]


def descriptor_windows(port):
    """[(addr, length)] of every window the descriptor in the flash of the radio on this debug link port names, [] when
    there is none, None when the radio can't be read. The stub refuses to erase a sector they lie in for an image
    without the codec (stub/f4stub.py guard_codec); this lets rfoundry tell before the radio is handed to the stub."""
    try:
        link = debuglink.Link(port)
    except Exception:
        return None
    try:
        d = bytes(link.read_mem(stockfw.DESC_ADDR, stockfw.DESC_LEN))
    except (debuglink.LinkError, OSError):
        return None
    finally:
        link.close()
    if len(d) != stockfw.DESC_LEN:
        return None
    w = struct.unpack("<%dI" % (stockfw.DESC_LEN // 4), d)
    if w[0] != stockfw.DESC_MAGIC or w[1] != stockfw.DESC_VERSION or not 0 < w[6] <= stockfw.DESC_WINDOWS:
        return []
    return [(w[3], w[4])] + [(w[7 + 3 * i], w[8 + 3 * i]) for i in range(w[6])]


def read_codec(link, board, progress=None):
    """(profile, segments) through an open debug link, or None when the radio's flash doesn't hold this codec or it
    can't be read. It tries each layout the board has in turn and stops at the first that matches, the ones the
    radio's descriptor names first (it isn't trusted: the windows are checked as for any other), so how long it takes
    isn't known up front: progress() is called after each window (segment) read."""
    desc = _described(link)
    cands = [(prof, form, wins) for prof in profiles_for(board) for form, wins in _forms(prof)]
    if desc:
        cands.sort(key=lambda c: not (c[1] is not None and c[1]["placement"]["tag"] == desc[0]
                                      and [a for a, _, _ in c[2]] == desc[1]))
    for prof, form, wins in cands:
        try:
            parts = []
            for a, n, _ in wins:
                parts.append((a, bytes(link.read_mem(a, n))))
                if progress:
                    progress()
        except (debuglink.LinkError, OSError):
            return None
        if _check(prof, form, parts):
            return prof, parts
    return None


def form_of(prof, parts):
    """The form (stockfw.forms, None for the stock windows) a copy's window addresses are in"""
    addrs = [a for a, _ in parts]
    for form, wins in _forms(prof):
        if addrs == [a for a, _, _ in wins]:
            return form
    raise ValueError("a codec copy at addresses no layout of %s has" % prof["name"])


def segments(prof, parts):
    """A copy (read_codec, load) as the merge writes it now: in the current placement with its descriptor. A copy in
    the stock windows or a former placement is moved there (stockfw.from_form)."""
    if not prof.get("placement"):
        return list(parts)
    out = stockfw.from_form(prof, form_of(prof, parts), parts)
    if out is None:
        raise ValueError("the codec copy doesn't check against %s" % prof["name"])
    return out


def path(board, root=None, codec_id=None, uid=None):
    """Where a copy is saved: one file per board, codec id and radio (its chip UID, when known)"""
    name = "%s-%s" % (board.id, codec_id or board.codec.id)
    if uid:
        name += "-" + uid.lower()
    return os.path.join(root or state.config_dir(), "codecs", name + ".bin")


def save(board, segments, root=None, uid=None):
    """The windows concatenated in profile order; their addresses and lengths come from the profile. uid: the chip UID
    of the radio it was read from (debuglink.probe), kept in the file name, so a copy is only ever matched to its own
    radio (find)."""
    p = path(board, root, uid=uid)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p + ".tmp", "wb") as f:
        f.write(b"".join(d for _, d in segments))
    os.replace(p + ".tmp", p)
    return p


class Copy:
    """A checked copy on this computer: path, uid (None for a copy saved without one, by an earlier rfoundry), profile
    and segments [(addr, bytes)]."""

    def __init__(self, path, uid, prof, parts):
        self.path, self.uid, self.prof, self.parts = path, uid, prof, parts


def _check_blob(board, blob):
    for prof in profiles_for(board):
        for form, wins in _forms(prof):
            parts, off = [], 0
            for a, n, _ in wins:
                parts.append((a, blob[off:off + n]))
                off += n
            if off == len(blob) and _check(prof, form, parts):
                return prof, parts
    return None


def copies(board, root=None):
    """Every copy of this board's codec saved on this computer that checks against a profile, newest first. A copy
    saved under an id an earlier release reported (board.codec_former) is found too: the codec is the same, only where
    it was read."""
    d = os.path.join(root or state.config_dir(), "codecs")
    try:
        names = os.listdir(d)
    except OSError:
        return []
    found = []
    for cid in [board.codec.id] + list(getattr(board, "codec_former", ())):
        stem = "%s-%s" % (board.id, cid)
        for name in names:
            if not name.endswith(".bin") or not name.startswith(stem):
                continue
            rest = name[len(stem):-4]
            if rest and not re.fullmatch(r"-[0-9a-f]{8,32}", rest):
                continue
            p = os.path.join(d, name)
            try:
                with open(p, "rb") as f:
                    blob = f.read()
                mtime = os.path.getmtime(p)
            except OSError:
                continue
            got = _check_blob(board, blob)
            if got:
                found.append((mtime, Copy(p, rest[1:] or None, *got)))
    found.sort(key=lambda x: -x[0])
    return [c for _, c in found]


def find(board, root=None, uid=None, strict=False):
    """The copy to use for a radio: with its chip UID known, the newest copy saved from that radio, else (not strict) the
    newest copy saved without a UID; with no UID known, the newest copy of any. None when there is none."""
    cs = copies(board, root)
    if uid is None:
        return cs[0] if cs else None
    mine = [c for c in cs if c.uid == uid.lower()]
    if mine:
        return mine[0]
    if strict:
        return None
    return next((c for c in cs if c.uid is None), None)


def load(board, root=None, uid=None):
    """(profile, segments) from the saved copy find() picks, checked against the profile, or None."""
    c = find(board, root, uid)
    return (c.prof, c.parts) if c else None


def from_radio(board, port=None, log=None, progress=None, uid=None):
    """Find the radio running RadioFoundry (or use port) and read its codec; None when there's no such radio or no codec.
    uid: read only the radio with this chip UID (the one probed), so a second radio on the computer is never read in
    its place. progress is read_codec's."""
    if not profiles_for(board):
        return None
    for p in [port] if port else (debuglink.find_ports() or []):
        try:
            link = debuglink.Link(p)
        except Exception as e:
            if log:
                log("could not open %s: %s" % (p, e))
            continue
        try:
            if uid is not None and (link.chip_uid() or "").lower() != uid.lower():
                continue
            got = read_codec(link, board, progress)
        finally:
            link.close()
        if got:
            return got
    return None
