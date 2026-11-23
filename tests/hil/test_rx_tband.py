# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Passive check on an already selected experimental receive-only trunked channel."""
import time
import pytest
from rfoundry_hil import skip


@pytest.mark.feature("rx_tband")
def test_tband_control_receive(radio):
    sections = ["core", "channel", "p25", "p25_trunk"]
    first = radio.view(sections)
    hz = int(first.get("p25t.cc_hz", "0"))
    if not 500000000 <= hz <= 503000000:
        skip("state", "select an RX-only T-band trunked test channel first")
    assert first.get("p25t.tx") == "0"
    start = int(first.get("p25t.blocks", "0"))
    valid = False
    for _ in range(15):
        v = radio.view(sections)
        for key in ("p25.tx_calls", "p25.tx_syms", "p25t.reg_tx", "p25t.aff_tx", "p25t.req_tx", "p25t.bursts"):
            assert v.get(key) == first.get(key), key
        valid |= (v.get("p25t.state") == "2" and v.get("p25t.unverified") == "0"
                  and 500000000 <= int(v.get("p25t.cc_hz", "0")) <= 503000000
                  and int(v.get("p25t.blocks", "0")) > start)
        time.sleep(1)
    assert valid, "no live control-channel reception established; programming a frequency is not proof of PLL lock"
