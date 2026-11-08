# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The TX guard (README.md, "Transmit frequencies"): layer 1 refuses a debug link request that would key outside the
rig's tx_mhz, layer 2 (Radio.tx_gate) sets the firmware's test TX limit: blocked outside radio_tx tests, the run's
transmit frequencies in them. No radio: these run
anywhere, and tools/ci.sh runs them."""
import struct
import types

import pytest

import hil_config
import rfoundry_hil as H


class FakeConfig:
    def __init__(self, allow_rf, tx_mhz, manual=False):
        self.option = types.SimpleNamespace(tx_mhz=tx_mhz)
        self._opts = {"--allow-rf": allow_rf, "--manual": manual}

    def getoption(self, name):
        return self._opts[name]


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
    """The core view's txlim key (built False: firmware without the test TX limit) and the limits set"""

    def __init__(self, built=True):
        self.built = built
        self.limits = []

    def view(self, sections):
        assert sections == ["core"]
        return {"txlim": "off"} if self.built else {}

    def tx_limit(self, block=False, mhz=()):
        self.limits.append("block" if block else tuple(mhz))


def gated(built=True):
    r = H.Radio.__new__(H.Radio)
    r.link = GateLink(built)
    r._txlim = None
    return r


def run_config(allow_rf):
    cfg = FakeConfig(allow_rf, [461.4625, 446.05, 467.375])
    cfg.option.dmr_freq = cfg.option.fm_mhz = 461.4625
    cfg.option.aprs_test_mhz = 446.05
    return cfg


def test_run_tx_mhz(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]       # each once, and not all of tx_mhz
    monkeypatch.setattr(hil_config, "CONFIG", run_config(False))
    assert hil_config.run_tx_mhz() == []


def test_run_tx_mhz_wattmeter(monkeypatch):
    """With --manual the wattmeter check's frequencies (default 445 and 146) join the run's, only those in tx_mhz"""
    cfg = run_config(True)
    cfg._opts["--manual"] = True
    cfg.option.tx_mhz = cfg.option.tx_mhz + [146.0]
    monkeypatch.setattr(hil_config, "CONFIG", cfg)
    assert hil_config.wattmeter_mhz() == [445.0, 146.0]
    assert hil_config.run_tx_mhz() == [146.0, 446.05, 461.4625]     # 445 isn't in tx_mhz
    cfg.option.wattmeter_mhz = "446.05, 461.4625"
    assert hil_config.wattmeter_mhz() == [446.05, 461.4625]
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]
    cfg._opts["--manual"] = False
    cfg.option.wattmeter_mhz = None
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]            # no person at the meter: none of them


def test_gate_blocks_outside_radio_tx(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    r = gated()
    assert r.tx_gate(False) and r.tx_gate(True) and r.tx_gate(False)
    r.tx_gate_end()
    assert r.link.limits == ["block", (446.05, 461.4625), "block", ()]


def test_gate_blocks_radio_tx_without_frequencies(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(False))
    r = gated()
    r.tx_gate(True)
    assert r.link.limits == ["block"]


def test_gate_without_the_limit(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    r = gated(False)
    assert not r.tx_gate(False) and not r.tx_gate(True)
    r.tx_gate_end()
    assert r.link.limits == []
