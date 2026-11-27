# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Receive-only extension admission: skips when the extended range is not enabled.
No live signal is needed. PTT is expected to be refused; --allow-rf is still required
for any test that presses PTT, following the existing HIL convention.
"""
import pytest
from rfoundry_hil import skip


@pytest.mark.feature("rx_ext")
@pytest.mark.needs_switch("VFO_FM")
@pytest.mark.radio_tx
def test_extended_receive_refuses_ptt(radio):
    radio.go_home()
    if not radio.in_band(501000000):
        skip("state", "501 MHz receive range is not enabled; turn on Settings > Advanced > RX past bands "
             "on a radio that supports this range")
    with radio.vfo_session():
        radio.link.vfo_set(501000000)
        v = radio.wait_for(lambda v: v.get("rx_hz") == "501000000" and v.get("mode") == "fm",
                           what="receive-only VFO at 501 MHz")
        assert v.get("tx_hz") == "501000000", v
        # can_tx reports board capability; the PTT result below checks this channel.
        downs = int(radio.link.view(["link"])["input.ptt_downs"])
        radio.link.ptt(True, max_ms=500)
        try:
            v = radio.wait_for(lambda v: v.get("tx_deny") == "band", what="PTT refused outside TX bands")
            assert v.get("state") != "tx", v
            assert int(radio.link.view(["link"])["input.ptt_downs"]) == downs + 1
        finally:
            radio.link.ptt(False)
