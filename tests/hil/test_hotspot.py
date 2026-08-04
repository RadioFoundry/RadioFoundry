# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""MMDVM hotspot mode without transmitting: GET_VERSION and GET_STATUS answered (tools/mmdvm_probe.py's framing),
the debug link still answers in hotspot mode, and hotspot mode is left through the debug link."""
import time

import pytest

GET_VERSION, GET_STATUS = 0x00, 0x01


def test_hotspot_version_status(radio):
    if "HOTSPOT" not in radio.features():
        pytest.skip("built without the hotspot")
    import mmdvm_probe
    with radio.released() as port:
        link = mmdvm_probe.Link(port)
        try:
            kind, f = link.request(GET_VERSION, timeout=2.0)
            assert kind == "reply" and len(f) > 4, f"GET_VERSION: {kind} {f!r}"
            assert b"OpenDigitalRadio" in f, f
            kind, f = link.request(GET_STATUS, timeout=1.0)
            assert kind == "reply" and len(f) >= 13, f"GET_STATUS: {kind} {f!r}"
        finally:
            link.s.close()
    # In hotspot mode now (until 20 s without MMDVM frames): the debug link still answers
    v = radio.wait_for(lambda x: x.get("hostlink") == "hotspot", 3, sections=("hotspot",), what="hotspot mode")
    assert v["hostlink"] == "hotspot"
    radio.ping()
    radio.link.hotspot_exit()
    radio.wait_for(lambda x: x.get("hostlink") == "idle", 5, sections=("hotspot",), what="hotspot left")
    radio.wait_for(lambda x: x.get("hotspot") == "0", 5, what="radio core back from hotspot")
    time.sleep(0.5)
