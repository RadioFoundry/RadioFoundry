# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The codec read back from a radio that runs RadioFoundry and saved on this computer, so that a later flash needs no stock
file: where an update erases part of it (the MD-380: RadioFoundry's image shares a flash sector with the codec, boards.py
keep_codec_supported), and on every other radio for when it can't be kept (a radio back on stock firmware, a flash
that stopped halfway, a radio in its bootloader). rfoundry reads every codec window over the debug link (READ_MEM,
docs/spec/debuglink.md 3.10) and checks each against the stock profile's SHA-256, either where stock firmware keeps
it or where the merge placed it (stockfw.placed_windows: the DM-1701's code in sector 11). The copy is checked again
when it is loaded, and a window that doesn't match its hash can't come out of this. The web flasher does the same
(site/static/js/flasher/codecsave.js), keeping its copy in the browser.

The saved file is the user's own codec, from their own radio: it stays on this computer (state.config_dir()).
"""
import hashlib
import os

from . import debuglink, state, stockfw


def profiles_for(board):
    """The stock profiles whose codec this board runs."""
    return [p for p in stockfw.PROFILES.values() if p.get("board") == board.id and p.get("codec_profile") == board.codec.id]


def _forms(prof):
    """Where a profile's codec can be in a radio: [(addr, length, sha256)] where stock firmware keeps it, and where the
    merge placed it when the profile moves the code."""
    return [stockfw.windows(prof)] + ([stockfw.placed_windows(prof)] if prof.get("placement") else [])


def _check(wins, parts):
    """[(addr, bytes)] when every window matches its hash and the code starts with the codec's signature, else None."""
    if len(parts) != len(wins):
        return None
    for (a, d), (wa, n, want) in zip(parts, wins):
        if a != wa or len(d) != n or hashlib.sha256(d).hexdigest() != want:
            return None
    if int.from_bytes(parts[0][1][:4], "little") != stockfw.CODEC_SIG:
        return None
    return parts


def read_codec(link, board):
    """(profile, segments) through an open debug link, or None when the radio's flash doesn't hold this codec or it
    can't be read."""
    for prof in profiles_for(board):
        for wins in _forms(prof):
            try:
                parts = [(a, bytes(link.read_mem(a, n))) for a, n, _ in wins]
            except (debuglink.LinkError, OSError):
                return None
            if _check(wins, parts):
                return prof, parts
    return None


def segments(prof, parts):
    """A copy (read_codec, load) as the merge writes it: windows at their stock addresses placed (stockfw.place),
    placed windows with their descriptor."""
    if not prof.get("placement"):
        return list(parts)
    if parts[0][0] == prof["code"][0]:
        return stockfw.place(prof, parts)
    return list(parts) + [(stockfw.DESC_ADDR, stockfw.descriptor(prof))]


def path(board, root=None):
    return os.path.join(root or state.config_dir(), "codecs", "%s-%s.bin" % (board.id, board.codec.id))


def save(board, segments, root=None):
    """The windows concatenated in profile order; their addresses and lengths come from the profile."""
    p = path(board, root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p + ".tmp", "wb") as f:
        f.write(b"".join(d for _, d in segments))
    os.replace(p + ".tmp", p)
    return p


def load(board, root=None):
    """(profile, segments) from the saved copy, checked against the profile, or None."""
    try:
        with open(path(board, root), "rb") as f:
            blob = f.read()
    except OSError:
        return None
    for prof in profiles_for(board):
        for wins in _forms(prof):
            parts, off = [], 0
            for a, n, _ in wins:
                parts.append((a, blob[off:off + n]))
                off += n
            if off == len(blob) and _check(wins, parts):
                return prof, parts
    return None


def from_radio(board, port=None, log=None):
    """Find the radio running RadioFoundry (or use port) and read its codec; None when there's no such radio or no codec."""
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
            got = read_codec(link, board)
        finally:
            link.close()
        if got:
            return got
    return None
