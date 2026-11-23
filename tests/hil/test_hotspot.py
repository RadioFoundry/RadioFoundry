# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""MMDVM hotspot mode without transmitting: GET_VERSION and GET_STATUS answered (tools/mmdvm_probe.py's framing), the
hotspot takes the DMR modem, the debug link still answers in hotspot mode, and hotspot mode is left through the debug
link.

The MMDVM client opens the port with DTR clear, as a host on Windows may. A client that raises DTR ends hotspot
mode when it closes the port (the DTR drop, docs/spec/mmdvm-protocol.md 5.1, hostlink_policy.c): with DTR set, the
radio would be back to normal before the debug link could look (hostlink.left=closed)."""
import struct
import time

import pytest

from rfoundry_hil import skip

GET_VERSION, GET_STATUS = 0x00, 0x01


@pytest.mark.feature("hotspot")
def test_hotspot_version_status(radio):
    if "HOTSPOT" not in radio.features():
        skip("not_built", "built without the hotspot")
    import mmdvm_probe
    before = radio.view(["hotspot"])
    starts0 = int(before.get("hs.starts", "0"))
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            kind, f = link.request(GET_VERSION, timeout=2.0)
            assert kind == "reply" and len(f) > 4, f"GET_VERSION: {kind} {f!r}"
            assert b"RadioFoundry" in f, f
            # The first frame takes the modem; once it has, GET_STATUS reports DMR slot 2 space (frame byte 8)
            space = 0
            for _ in range(6):
                kind, f = link.request(GET_STATUS, timeout=1.0)
                assert kind == "reply" and len(f) >= 13, f"GET_STATUS: {kind} {f!r}"
                space = f[8]
                if space > 0:
                    break
                time.sleep(0.25)
            assert space > 0, "the hotspot never took the modem (GET_STATUS slot 2 space stayed 0)"
        finally:
            link.s.close()
    # In hotspot mode now (until 20 s without MMDVM frames): the debug link still answers
    v = radio.wait_for(lambda x: x.get("hostlink") == "hotspot" and x.get("hs.running") == "1", 3,
                       sections=("hotspot",), what="hotspot mode with the hotspot running")
    assert int(v["hs.starts"]) == starts0 + 1, v
    assert radio.core().get("hotspot") == "1"
    radio.ping()
    radio.link.hotspot_exit()
    v = radio.wait_for(lambda x: x.get("hostlink") == "idle", 5, sections=("hotspot",), what="hotspot left")
    assert v.get("hostlink.left") == "user" and v.get("hs.running") == "0", v
    radio.wait_for(lambda x: x.get("hotspot") == "0", 5, what="radio core back from hotspot")
    time.sleep(0.5)


SET_CONFIG, SET_FREQ = 0x02, 0x04


def _set_freq(radio, hz, rf_level):
    """SET_CONFIG (color code 1, DMR only) and SET_FREQ with an RF level, then the port back to the debug link. The
    plugin gates TX for this test: the hotspot sets its power level but the keying is refused, so nothing goes on air"""
    import mmdvm_probe
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            assert link.request(GET_VERSION, timeout=2.0)[0] == "reply"
            cfg = bytes([0x80, 0x02, 10, 2, 128, 128, 1, 0, 128, 0, 128, 0, 0, 128, 128] + [0] * 8)    # DMR, CC 1
            assert link.request(SET_CONFIG, cfg)[0] == "ack"
            freq = bytes([0]) + struct.pack("<II", hz, hz) + bytes([rf_level]) + struct.pack("<I", hz)
            assert link.request(SET_FREQ, freq)[0] == "ack"
        finally:
            link.s.close()


@pytest.mark.feature("hotspot")
def test_hotspot_rf_level_power(radio):
    """The host's RF level picks the hotspot's power (docs/spec/mmdvm-protocol.md, SET_FREQ): FF, the host's default,
    keeps the radio's own power and follows a power change on the radio; 0-FE spread over the levels and ignore one.
    FF once keyed the top level (45 W on a radio set to 5 W)"""
    if "HOTSPOT" not in radio.features():
        skip("not_built", "built without the hotspot")
    core = radio.core()
    n = int(core.get("power_count") or 0)
    if n < 2:
        skip("no_hw", "one power level: nothing to tell apart")
    hz = int(core["tx_hz"])
    radio.link.power_level("lowest")
    _set_freq(radio, hz, 0xFF)
    v = radio.wait_for(lambda x: x.get("hotspot") == "1", 5, what="hotspot mode")
    if "hs_power" not in v:
        skip("not_built", "firmware without the hs_power view key")
    assert v["hs_power"] == "0", f"RF level FF keyed level {v['hs_power']}, the radio is at 0: {v}"
    # A power change on the radio applies at once while the host's level is FF
    radio.link.power_step(1)
    radio.wait_for(lambda x: x.get("power") == "1" and x.get("hs_power") == "1", 3, what="hotspot follows the radio")
    # A level the host sets: spread over the levels (0 the lowest, FE the highest), a power change ignored
    _set_freq(radio, hz, 0)
    radio.wait_for(lambda x: x.get("hs_power") == "0", 3, what="RF level 0 keys the lowest level")
    radio.link.power_step(-1)
    radio.wait_for(lambda x: x.get("power") == "0", 3, what="radio power back to 0")
    radio.link.power_step(1)
    v = radio.wait_for(lambda x: x.get("power") == "1", 3, what="radio power 1")
    time.sleep(0.3)
    assert radio.core().get("hs_power") == "0", "a radio power change moved the host's level"
    mid = (128 * (n - 1) + 127) // 255
    _set_freq(radio, hz, 128)
    radio.wait_for(lambda x: x.get("hs_power") == str(mid), 3, what=f"RF level 128 keys level {mid} of {n}")
    _set_freq(radio, hz, 0xFE)
    radio.wait_for(lambda x: x.get("hs_power") == str(n - 1), 3, what="RF level FE keys the top level")
    radio.link.hotspot_exit()
    radio.wait_for(lambda x: x.get("hotspot") == "0", 25, what="radio core back from hotspot")
