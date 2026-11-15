# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Read-only AES firmware diagnostics. Interoperability is checked manually."""
import pytest

from rfoundry_hil import as_int, skip


@pytest.mark.feature("dmr_aes256")
def test_aes_diagnostics(radio):
    if "DMR_AES256" not in radio.features():
        skip("not_built", "built without AES-256 voice")
    view = radio.view(["dmr"])
    assert as_int(view["dmr.aes256"]) == 1
    assert 0 <= as_int(view["dmr.aes_tx_slot"]) <= 2
    assert as_int(view["dmr.aes_rx_ready"]) in (0, 1)
