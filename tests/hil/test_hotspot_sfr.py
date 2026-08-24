# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The MMDVM hotspot's SFR mode without transmitting (a CONFIG_SFR=y build): GET_VERSION announces the SFR, the
duplex flag with RX = TX turns it on (GET_STATUS grows to 121 bytes, both space bytes carry the outbound slot's
space, the debug link shows sfr.on=1), the simplex flag turns it off again (docs/spec/mmdvm-protocol.md 6,
docs/spec/sfr.md). The radio keys only for something heard or sent: this test sends nothing, but a call heard on
the channel would be repeated, so it is marked radio_tx (needs --allow-rf) and runs on the radio's current channel
frequency (the bench's). It expects a quiet channel (sfr.tx_starts stays put). The MMDVM client keeps DTR clear (see
test_hotspot.py)."""
import struct
import time

import pytest

GET_VERSION, GET_STATUS, SET_CONFIG, SET_FREQ = 0x00, 0x01, 0x02, 0x04
SFR_DEBUG = 0xD5


def config(simplex, cc=1):
    return bytes([0x80 if simplex else 0x00, 0x02, 10, 2, 128, 128, cc, 0, 128, 0, 128, 0, 0, 128, 128] + [0] * 8)


def freq(rx, tx):
    return bytes([0]) + struct.pack("<II", rx, tx) + bytes([0]) + struct.pack("<I", tx)


@pytest.mark.radio_tx
def test_hotspot_sfr_mode(radio):
    feats = radio.features()
    if "SFR" not in feats or "HOTSPOT" not in feats:
        pytest.skip("built without the SFR (make CONFIG_SFR=y)")
    import mmdvm_probe
    hz = int(radio.core()["rx_hz"])
    tx0 = None
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            kind, f = link.request(GET_VERSION, timeout=2.0)
            assert kind == "reply" and b" SFR TS1>TS2 " in f, f
            kind, f = link.request(SET_CONFIG, config(simplex=False))
            assert kind == "ack", f
            kind, f = link.request(SET_FREQ, freq(hz, hz))
            assert kind == "ack", f
            st = None
            for _ in range(8):
                kind, st = link.request(GET_STATUS)
                assert kind == "reply", st
                if len(st) == 121:
                    break
                time.sleep(0.25)
            assert len(st) == 121, f"SFR status bytes missing: {st.hex()}"
            assert st[7] == st[8] == 16, st.hex()
            assert st[5] & 0x80 == 0, "inbound busy on a quiet channel"
            kind, f = link.request(SFR_DEBUG, bytes([0, 0]))
            assert kind == "ack", f
            kind, f = link.request(SFR_DEBUG, bytes([0]))
            assert kind == "nak" and f[4] == 4, f
        finally:
            link.s.close()
    v = radio.wait_for(lambda x: x.get("hostlink") == "hotspot" and x.get("sfr.on") == "1", 3,
                       sections=("hotspot",), what="SFR mode on")
    tx0 = v.get("sfr.tx_starts")
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            kind, f = link.request(SET_CONFIG, config(simplex=True))
            assert kind == "ack", f
            kind, st = link.request(GET_STATUS)
            assert kind == "reply" and len(st) == 13, st
        finally:
            link.s.close()
    v = radio.wait_for(lambda x: x.get("sfr.on") == "0", 3, sections=("hotspot",), what="SFR mode off")
    assert v.get("sfr.tx_starts") == tx0, "the SFR keyed on a quiet channel"
    radio.link.hotspot_exit()
    radio.wait_for(lambda x: x.get("hostlink") == "idle", 5, sections=("hotspot",), what="hotspot left")
