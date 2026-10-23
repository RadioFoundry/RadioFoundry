# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The bootloader key from a radio that runs RadioFoundry, so an update needs no stock file. The stock bootloader holds the key
as one plain block: rfoundry reads the bootloader sectors over the debug link (READ_MEM, docs/spec/debuglink.md 3.10)
and takes the block whose SHA-256 is the board's key_sha256. No offset or layout is assumed, and a key that doesn't
match the hash can't come out of this. The web flasher does the same (site/static/js/flasher/bootkey.js).
"""
import hashlib

from . import debuglink
from .stockfw import KEY_LEN


def find_key(region, want_sha256, key_len=KEY_LEN):
    """The key-sized block of region whose SHA-256 is want_sha256, or None. 4-byte aligned offsets first."""
    offsets = range(len(region) - key_len + 1)
    for o in [o for o in offsets if o % 4 == 0] + [o for o in offsets if o % 4]:
        if hashlib.sha256(region[o:o + key_len]).hexdigest() == want_sha256:
            return bytes(region[o:o + key_len])
    return None


def read_key(link, board):
    """The key through an open debug link, or None when the board has none or the radio can't give it."""
    if not board.key_sha256 or not board.protected:
        return None
    start, end = board.protected[0]
    try:
        region = link.read_mem(start, end - start)
    except (debuglink.LinkError, OSError):
        return None
    return find_key(region, board.key_sha256)


def from_radio(board, port=None, log=None):
    """Find the radio running RadioFoundry (or use port) and read its key; None when there's no such radio."""
    if not board.key_sha256:
        return None
    ports = [port] if port else (debuglink.find_ports() or [])
    for p in ports:
        try:
            link = debuglink.Link(p)
        except Exception as e:
            if log:
                log("could not open %s: %s" % (p, e))
            continue
        try:
            key = read_key(link, board)
        finally:
            link.close()
        if key:
            return key
    return None
