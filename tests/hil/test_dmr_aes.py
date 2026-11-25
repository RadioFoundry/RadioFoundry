# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR AES-256 on the radio (docs/spec/dmr-voice.md 5.1).

test_aes_missing_key_refuses_ptt (radio_tx, codeplug_write; no second radio, no bench): the simplex DMR channel on the
DMR test frequency (--dmr-freq, as the other DMR TX tests use) gets an AES tag (kind 5) bound to a free system of type 5
(DMR AES) and a key ID no traffic key holds. PTT must be refused with tx_deny=aes_key on the debug link and the state
must never show tx; data_deny and packet_deny must say aes_key too (a radio check or text is refused with that
reason, not sent). The records are taken out again and the channel selected anew. radio_tx only as a guard: were the
refusal broken, PTT would key --dmr-freq, which the rig may send on, so the run's TX limit never refuses the PTT in the
key's place.

test_aes_signaling_tx (radio_tx, codeplug_write, the bench modem listens): the same channel with a traffic key
(docs/spec/dmr-data.md 13). Signaling goes in the clear: a radio check of the bench is on air (three tries, no answer).
Packet data is held back: a text fails as blocked (data.event sms_blocked) and no data header from us is on air. The
test owns system CP_SYS and the traffic key family's last entry (TRAFFIC_SLOT) for the run.
"""
import struct
import time

import pytest

from rfoundry_debug import as_int, DebugLink, CPKV_SYSTEM, CPKV_CH_TAG
from rfoundry_hil import skip
from hil_config import rig, tx_allowed
from test_dmr_data import (BENCH_SRC, CSBK_RADIO_CHECK, FID_MOTOROLA, _deltas, _heard_headers, _radio_sends,
                           heard_tries)

KIND_DMR_AES = SYSTYPE_DMR_AES = 5
AES_ALG = 0x25
CP_SYS = 251            # a system index the test owns for the run (checked free)
KEY_ID = 0xA5           # a PI key ID no traffic key holds on the test system
TRAFFIC_SLOT = 0x1CFF   # the traffic key family's last entry (docs/spec/channel-modes.md 5.4)


def _dmr_channel(radio, freq):
    """The simplex DMR channel on freq (MHz), selected: (position, zone size, channel number, channel view)"""
    pos = radio.find_channel(round(freq * 1e6), "dmr", simplex=True)
    size = as_int(radio.core().get("zone_size")) or 0
    if size < 2:
        skip("state", "the zone has one channel: the test steps off the channel to reload it")
    c = as_int(radio.core().get("channel_number"))
    if not c:
        skip("state", f"the channel on {freq:.5f} MHz has no codeplug number: the AES tag needs one")
    return pos, size, c, radio.view(["channel"])


@pytest.mark.feature("dmr_aes")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_aes_missing_key_refuses_ptt(radio, pytestconfig):
    if "DMR_AES" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_AES")
    L = radio.link
    freq = rig(pytestconfig, "dmr-freq")
    if not tx_allowed(None, freq):
        skip("opt_in", f"PTT refusal not checked: --dmr-freq {freq:.5f} MHz isn't in tx_mhz")
    pos, size, c, ch = _dmr_channel(radio, freq)
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
        # The refusal must be the key's: PTT goes out only on the run's TX frequency
        tx_hz = as_int(radio.core().get("tx_hz")) or 0
        assert abs(tx_hz - round(freq * 1e6)) < 100, f"TX on {tx_hz / 1e6:.5f} MHz, not --dmr-freq {freq:.5f} MHz"
        # Data is refused for the same reason, and says so before a send (radio check, text: no "No answer")
        core = radio.core()
        assert core.get("data_deny") == "aes_key" and core.get("packet_deny") == "aes_key", \
            f"data_deny {core.get('data_deny')!r} packet_deny {core.get('packet_deny')!r}, not aes_key"
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
        assert deny == "aes_key", f"tx_deny {deny!r}, not aes_key: PTT was refused for another reason than the key"
    finally:
        L.ptt(False)
        for k in keys:
            L.cpkv_remove(k)
        select()


@pytest.mark.feature("dmr_aes")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_aes_signaling_tx(radio, sdr_bench, pytestconfig):
    feats = radio.features()
    for f in ("DMR_AES", "DMR_SIGNALING", "DMR_SMS"):
        if f not in feats:
            skip("not_built", f"built without CONFIG_{f}")
    L = radio.link
    freq, cc = rig(pytestconfig, "dmr-freq"), rig(pytestconfig, "dmr-cc")
    if not tx_allowed(None, freq):
        skip("opt_in", f"--dmr-freq {freq:.5f} MHz isn't in tx_mhz")
    radio.go_home()
    pos, size, c, ch = _dmr_channel(radio, freq)
    radio.lowest_power()
    own = as_int(radio.view(["dmr"]).get("dmr.own_id")) or 0
    if own == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
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
        L.cpkv_set(keys[0], DebugLink.system_record(SYSTYPE_DMR_AES, "HIL AES", b"\x01", identity_len=1))
        L.cpkv_set(TRAFFIC_SLOT, struct.pack("<BBHB", CP_SYS, AES_ALG, KEY_ID, 0) + bytes(range(32)))
        L.cpkv_set(keys[1], DebugLink.tag_record(KIND_DMR_AES, CP_SYS, ch.get("ch.name", ""),
                                                 flags=0x01 | 0x02, params=struct.pack("<BB", AES_ALG, KEY_ID)))
        select()
        tx_hz = as_int(radio.core().get("tx_hz")) or 0
        assert abs(tx_hz - round(freq * 1e6)) < 100, f"TX on {tx_hz / 1e6:.5f} MHz, not --dmr-freq {freq:.5f} MHz"
        # The MI reservation is saved a moment after the channel loads (aes_wait), then data may go
        v = radio.wait_for(lambda v: v.get("data_deny") == "none", 10, sections=("core",), what="data_deny none")
        assert v.get("packet_deny") == "data_encrypted", f"packet_deny {v.get('packet_deny')!r}, not data_encrypted"

        # Signaling: the radio check goes on air in the clear
        before = radio.data_view()
        heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: L.radio_check(BENCH_SRC), 14,
                                lambda v: v.get("data.signal_state") in ("ok", "failed"))
        after = radio.data_view()
        d = _deltas(before, after, ["data.radio_checks_sent", "data.tx_failed"])
        print("radio check deltas:", d)
        assert after["data.signal_kind"] == "radio_check" and after["data.signal_state"] == "failed", after
        assert d["data.radio_checks_sent"] == 3 and d["data.tx_failed"] == 0, d
        heard_tries(heard, CSBK_RADIO_CHECK, d["data.radio_checks_sent"],
                    lambda x: x[1] == FID_MOTOROLA and x[2] == 0x00 and x[3] == own and x[4] == BENCH_SRC,
                    "radio check on an AES channel")

        # Packet data: a text is blocked, never on air
        before = radio.data_view()
        heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: L.sms(BENCH_SRC, "RadioFoundry HIL AES", ack=False),
                                6, lambda v: v.get("data.sms_state") in ("ok", "failed"))
        after = radio.data_view()
        d = _deltas(before, after, ["data.sms_sent", "data.tx_ok"])
        print("text deltas:", d)
        assert after["data.sms_state"] == "failed" and after["data.event"] == "sms_blocked", after
        assert d["data.sms_sent"] == 0, d
        ours = [h for h in _heard_headers(heard) if h[1] == own]
        assert not ours, f"a data header from us was on air (destination, source): {ours}"
    finally:
        for k in keys + (TRAFFIC_SLOT,):
            L.cpkv_remove(k)
        select()
