# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Passive observations of a receive-envelope diagnostic image; never keys RF."""
import time
import pytest
from rfoundry_hil import skip


@pytest.mark.feature("rx_envelope_diag")
def test_envelope_receive_only(radio):
    first = radio.view(["core", "board"])
    if first.get("board.envelope_diag") != "1":
        skip("not_built", "requires the receive-envelope diagnostic image")
    assert first["board.pin.tx"] == "000"
    assert first["state"] != "tx"
    time.sleep(.1)
    after = radio.view(["core", "board"])
    assert after["board.pin.tx"] == "000"
    assert after["last_tx_ms"] == first["last_tx_ms"]
    assert after["state"] != "tx"
