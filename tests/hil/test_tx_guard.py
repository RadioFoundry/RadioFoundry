# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The TX guard (README.md, "Transmit frequencies"): layer 1 refuses a debug link request that would key outside the
rig's tx_mhz, layer 2 (Radio.tx_gate) keeps the radio's TX inhibit on outside radio_tx tests. No radio: these run
anywhere, and tools/ci.sh runs them."""
import struct
import types

import pytest

import hil_config
import rfoundry_hil as H


class FakeConfig:
    def __init__(self, allow_rf, tx_mhz):
        self.option = types.SimpleNamespace(tx_mhz=tx_mhz)
        self._rf = allow_rf

    def getoption(self, name):
        assert name == "--allow-rf"
        return self._rf


class FakeLink:
    """The views the guard reads, and the requests that got through"""

    def __init__(self, tx_mhz=461.4625, mode="dmr", aprs_mhz=0.0, via="both"):
        self.views = {"core": {"tx_hz": str(round(tx_mhz * 1e6)), "mode": mode},
                      "aprs": {"aprs.freq_hz": str(round(aprs_mhz * 1e6)), "aprs.block": "ok" if aprs_mhz else "no_freq"},
                      "aprs_set": {"set.aprs_via": via}}
        self.sent = []

    def view(self, sections):
        return dict(self.views[sections[0]])


@pytest.fixture
def rig(monkeypatch):
    sent = []
    monkeypatch.setattr(H, "_request", lambda link, cmd, payload=b"", *a, **k: sent.append((cmd, bytes(payload))))
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(True, [461.4625, 446.05]))
    return sent


def ptt(down):
    return H.PTT, struct.pack("<BH", 1 if down else 0, 1000)


@pytest.mark.parametrize("cmd,payload,where", [
    (*ptt(True), "channel"),
    (*ptt(False), None),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ptt"], H.PRESS, 0), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ptt"], H.UP, 0), None),
    (H.KEY, struct.pack("<BBH", 3, H.ACTIONS["ptt"], 500), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["emergency"], H.LONG, 0), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ok"], H.PRESS, 0), None),
    (H.RADIO, bytes([H.OP_TONE_BURST, 1, 0, 0]), "channel"),
    (H.RADIO, bytes([H.OP_TONE_BURST, 0, 0, 0]), None),
    (H.DMR_DATA, bytes([H.DATA_OP_SMS]) + b"x", "channel"),
    (H.DMR_DATA, bytes([H.DATA_OP_EMERGENCY, 1]), "channel"),
    (H.DMR_DATA, bytes([H.DATA_OP_EMERGENCY, 0]), None),
    (H.APRS, bytes([H.APRS_OP_SEND_NOW]), "position"),
    (H.APRS, bytes([H.APRS_OP_STATUS]) + b"x", "aprs"),
    (H.APRS, bytes([4]) + b"Q0TST", None),       # TEST_CALL: a RAM-only callsign, no transmission
    (9, b"", None),                     # GET_VIEW
])
def test_keys_tx(cmd, payload, where):
    assert H.keys_tx(cmd, payload) == where


def test_allowed_channel_passes(rig):
    H._guarded_request(FakeLink(461.4625), *ptt(True))
    assert len(rig) == 1


def test_split_channel_refused(rig):
    # found by its RX frequency, the channel transmits elsewhere
    with pytest.raises(RuntimeError, match="467.37500 MHz"):
        H._guarded_request(FakeLink(467.375), H.KEY, struct.pack("<BBH", 3, H.ACTIONS["ptt"], 500))
    assert rig == []


def test_requests_that_dont_key_pass_anywhere(rig):
    H._guarded_request(FakeLink(467.375), *ptt(False))
    H._guarded_request(FakeLink(467.375), H.RADIO, bytes([H.OP_TONE_BURST, 0, 0, 0]))
    assert len(rig) == 2


def test_aprs_frequency_and_dmr_channel(rig):
    H._guarded_request(FakeLink(461.4625, "fm", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(FakeLink(461.4625, "fm", aprs_mhz=144.39), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    with pytest.raises(RuntimeError, match="467.37500"):     # positions over DMR go on the DMR channel
        H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    assert len(rig) == 1


def test_aprs_position_by_its_way_out(rig):
    send = (H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="dmr"), *send)    # over DMR: no AFSK
    H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05, via="rf"), *send)      # by AFSK: the channel idles
    with pytest.raises(RuntimeError, match="467.37500"):
        H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05, via="dmr"), *send)
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="rf"), *send)
    link = FakeLink(461.4625, "dmr", aprs_mhz=144.39)
    del link.views["aprs_set"]["set.aprs_via"]      # a firmware that doesn't report it gets both checked
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(link, *send)
    with pytest.raises(RuntimeError, match="144.39000"):      # a message goes by AFSK whatever the via
        H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="dmr"), H.APRS, bytes([H.APRS_OP_MESSAGE]) + b"x")
    H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_STATUS]) + b"x")
    assert len(rig) == 3


def test_aprs_frequency_only_when_rf_aprs_can_send(rig):
    link = FakeLink(461.4625, "fm", aprs_mhz=144.39)
    link.views["aprs"]["aprs.block"] = "not_amateur"
    H._guarded_request(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    assert len(rig) == 1


def test_no_check_without_allow_rf(rig, monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(False, []))
    H._guarded_request(FakeLink(467.375), *ptt(True))      # layer 2 keeps a radio without --allow-rf off the air
    assert len(rig) == 1


class GateLink:
    def __init__(self, inhibit):
        self.inhibit = inhibit
        self.sets = []

    def view(self, sections):
        assert sections == ["features"]
        return {} if self.inhibit is None else {"set.tx_inhibit": self.inhibit}

    def setting(self, name, value):
        assert name == "tx_inhibit"
        self.sets.append(value)
        self.inhibit = str(value)


def gated(inhibit):
    r = H.Radio.__new__(H.Radio)
    r.link = GateLink(inhibit)
    return r


def test_gate_inhibits_outside_radio_tx_and_puts_it_back():
    r = gated("0")
    assert r.tx_gate(False) and r.link.inhibit == "1"
    assert r.tx_gate(True) and r.link.inhibit == "0"
    r.tx_gate(False)
    r.tx_gate_end()
    assert r.link.inhibit == "0" and r.link.sets == [1, 0, 1, 0]


def test_gate_keeps_an_inhibit_the_owner_set():
    r = gated("1")
    r.tx_gate(True)                     # a radio_tx test lifts it
    r.tx_gate_end()
    assert r.link.inhibit == "1"


def test_gate_without_the_setting():
    r = gated(None)
    assert not r.tx_gate(False) and r.link.sets == []
    r.tx_gate_end()
