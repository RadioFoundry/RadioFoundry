# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Read-only receive AGC diagnostics; listening and timing checks remain owner-operated."""
import pytest
import rfoundry_hil


@pytest.mark.feature("rx_agc")
def test_receive_agc_diagnostics(radio):
    audio = radio.view(["audio"])
    if int(audio.get("agc.built", 0)) != 1:
        rfoundry_hil.skip("not_built", "CONFIG_RX_AGC is off or firmware predates receive AGC")
    for mode in range(3):
        for field in ("enabled", "valid", "acquiring", "cached"):
            assert int(audio[f"agc.{mode}.{field}"]) in (0, 1)
        for field in ("input", "output"):
            assert -96 <= int(audio[f"agc.{mode}.{field}"]) <= 0
        assert -48 <= int(audio[f"agc.{mode}.gain"]) <= 48
