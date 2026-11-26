# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Contact lists (docs/spec/contact-lists.md), radio only, nothing transmitted and nothing written: the selection the
radio uses on the channel in use (`dmr.names_sel` of the dmr view, debuglink.md 3.2) against the codeplug's own
keys, read over the debug link (CPKV): the channel's network byte (key 0x2002, byte channel - 1) and that network's
two list bytes (key 0x1F00 + network). A network byte counts only when that system's record (key 0x0100 + network,
byte 0) is the Conventional type (6); a list byte other than 0-31 or 254 reads as none. A channel with no network, or
no codeplug number (the VFO), selects nothing (255, 255). The test reads the codeplug the radio holds: it needs a
channel that names a network, and skips when none of the zone's channels does (the editor's Migrate button makes one),
and when the codeplug holds no contact list key at all: that one is not migrated and the radio names calls the old way
(contact-lists.md 6)."""
import struct

import pytest

from rfoundry_debug import as_int, CPKV, CPKV_GET, CPKV_CH_TAG
from rfoundry_hil import skip

KEY_SYSTEM = 0x0100         # + system index: the system's record, byte 0 its type (channel-modes.md 7.1)
KEY_LIST_NAME = 0x1E00      # + list number 0-31
KEY_SYS_LISTS = 0x1F00      # + system index: [primary, secondary] (contact-lists.md 3)
KEY_LIST_TABLES = (0x2000, 0x2001, 0x2002, 0x2003)    # membership (DMR, P25), channel network, check
KEY_CH_NET = 0x2002         # byte i is channel i + 1: its network system, or 0xFF
TYPE_CONVENTIONAL = 6       # the system type of a network (channel-modes.md 7.5)
IDDB = 254                  # the ID database, a selection only
NONE = 255                  # no list


def key_byte(link, key, off):
    """One byte of a key's value from the codeplug settings area, None when the key is absent or shorter. A
    shortened value reads 0xFF past its end (contact-lists.md 3.1), so a short one is NONE, not None, for the caller
    that wants that"""
    try:
        r = link.request(CPKV, struct.pack("<BHH", CPKV_GET, key, off))[1]
    except Exception as e:     # an absent key is an error status of the debug link
        if getattr(e, "status", None) == 4:
            return None
        raise
    return r[2] if len(r) > 2 else None


def any_list_key(link):
    """Whether the codeplug holds a contact list key (0x1E00-0x20FF). With none it is not migrated, and the radio names
    calls through every contact then the ID database, whatever the selection (contact-lists.md 6)"""
    keys = list(KEY_LIST_TABLES) + [KEY_LIST_NAME + n for n in range(32)] + [KEY_SYS_LISTS + n for n in range(255)]
    return any(link.cpkv_get(k) is not None for k in keys)


def selection(link, ch):
    """The two list bytes the codeplug gives channel ch (1-1024) when it has no tag naming a system. A network byte
    that doesn't name a Conventional system is no network, and a list byte that is neither a list (0-31) nor the ID
    database reads as none"""
    net = key_byte(link, KEY_CH_NET, ch - 1)
    if net is None or net == NONE or key_byte(link, KEY_SYSTEM + net, 0) != TYPE_CONVENTIONAL:
        return (NONE, NONE), None
    out = []
    for off in (0, 1):
        b = key_byte(link, KEY_SYS_LISTS + net, off)
        out.append(b if b is not None and (b < 32 or b == IDDB) else NONE)
    return tuple(out), net


@pytest.mark.feature("contact_lists")
def test_names_selection_matches_codeplug(radio):
    if "CONTACT_LISTS" not in radio.features():
        skip("not_built", "built without CONFIG_CONTACT_LISTS")
    if "DMR" not in radio.features():
        skip("not_built", "built without CONFIG_DMR: no dmr view")
    radio.go_home()
    L = radio.link
    v = radio.view(["core", "dmr"])
    if v.get("vfo") == "1":
        # The VFO has no channel record: no network, no names (contact-lists.md 5.1)
        assert v.get("dmr.names_sel") == f"{NONE},{NONE}", v
        skip("state", "the radio is on the VFO: select a channel that names a network")
    ch = as_int(v.get("channel_number"))
    if not ch:
        skip("state", "the channel has no codeplug number")
    if L.cpkv_get(CPKV_CH_TAG + ch - 1) is not None:
        skip("state", f"channel {ch} has a tag: its system's lists win over a network (contact-lists.md 5.1)")
    if not any_list_key(L):
        skip("state", "the codeplug holds no contact list key (not migrated): the radio uses the old lookup; "
                      "press Migrate in the Codeplug Editor")
    want, net = selection(L, ch)
    if net is None:
        skip("state", f"channel {ch} names no network (or one that isn't a Conventional system): set a network on a "
                      "channel in the Codeplug Editor")
    got = v.get("dmr.names_sel")
    assert got == f"{want[0]},{want[1]}", \
        f"channel {ch}, network {net}: the radio selects {got}, the codeplug says {want}"
