# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""MMDVM hotspot mode without transmitting: GET_VERSION and GET_STATUS answered (tools/mmdvm_probe.py's framing), the
hotspot takes the DMR modem, the debug link still answers in hotspot mode, and hotspot mode is left through the debug
link.

The MMDVM client opens the port with DTR clear, as MMDVMHost on Windows does. A client that raises DTR ends hotspot
mode when it closes the port (the DTR drop, docs/spec/mmdvm-protocol.md 5.1, hostlink_policy.c): with DTR set, the
radio would be back to normal before the debug link could look (hostlink.left=closed)."""
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
