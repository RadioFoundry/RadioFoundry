# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR AES-256 on the radio (docs/spec/dmr-voice.md 5.1).

test_aes_missing_key_refuses_ptt (radio_tx, codeplug_write; no second radio, no bench): the zone's first DMR channel
gets an AES tag (kind 5) bound to a free system of type 5 (DMR AES) and a key ID no traffic key holds. PTT must be
refused with tx_deny=aes_key on the debug link and the state must never show tx. The records are taken out again and
the channel selected anew. radio_tx only as a guard: were the refusal broken, PTT would key the channel's own TX
frequency, so the test runs only where the rig may send there (tx_mhz).
"""
import struct
import time

import pytest

from rfoundry_debug import as_int, DebugLink, CPKV_SYSTEM, CPKV_CH_TAG
from rfoundry_hil import skip
from hil_config import tx_allowed

KIND_DMR_AES = SYSTYPE_DMR_AES = 5
AES_ALG = 0x25
CP_SYS = 251            # a system index the test owns for the run (checked free)
KEY_ID = 0xA5           # a PI key ID no traffic key holds on the test system


def _dmr_channel(radio):
    L = radio.link
    size = as_int(radio.core().get("zone_size")) or 0
    if size < 2:
        skip("state", "the zone has one channel: the test steps off the channel to reload it")
    for pos in range(size):
        L.zone_pos(pos)
        time.sleep(0.3)
        c = as_int(radio.core().get("channel_number"))
        ch = radio.view(["channel"])
        if c and ch.get("ch.mode") == "dmr":
            return pos, size, c, ch
    skip("state", "no DMR channel in the zone in use: select a zone that has one")


@pytest.mark.feature("dmr_aes")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_aes_missing_key_refuses_ptt(radio):
    if "DMR_AES" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_AES")
    L = radio.link
    if radio.core().get("vfo") == "1":
        L.vfo(False)
    pos, size, c, ch = _dmr_channel(radio)
    mhz = (as_int(radio.core().get("tx_hz")) or 0) / 1e6
    if not tx_allowed(None, mhz):
        skip("opt_in", f"PTT refusal not checked: the channel's TX frequency {mhz:.5f} MHz isn't in tx_mhz")
    keys = (CPKV_SYSTEM + CP_SYS, CPKV_CH_TAG + c - 1)
    for k in keys:
        if L.cpkv_get(k) is not None:
            skip("state", f"codeplug key {k:#06x} is in use: the test won't write over the user's set-up")
    other = (pos + 1) % size

    def select():
        L.zone_pos(other)
        time.sleep(0.5)
        L.zone_pos(pos)
        time.sleep(0.5)

    try:
        # The system: type 5, its one identity byte; the tag: AES, the PI key ID, on the channel as it is
        L.cpkv_set(keys[0], DebugLink.system_record(SYSTYPE_DMR_AES, "HIL AES", b"\x01", identity_len=1))
        L.cpkv_set(keys[1], DebugLink.tag_record(KIND_DMR_AES, CP_SYS, ch.get("ch.name", ""),
                                                 flags=0x01 | 0x02, params=struct.pack("<BB", AES_ALG, KEY_ID)))
        select()
        L.ptt(True, 3000)
        try:
            end = time.monotonic() + 2.0
            deny = None
            while time.monotonic() < end:
                v = radio.core()
                assert str(v.get("state", "")).lower() != "tx", "PTT keyed an AES channel without its key"
                deny = v.get("tx_deny") or deny
                time.sleep(0.1)
        finally:
            L.ptt(False)
        assert deny == "aes_key", f"tx_deny {deny!r}, not aes_key"
    finally:
        L.ptt(False)
        for k in keys:
            L.cpkv_remove(k)
        select()
