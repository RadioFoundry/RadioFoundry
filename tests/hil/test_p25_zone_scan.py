# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Read-only scan diagnostics; no channel changes, programming writes or RF requests."""
import pytest
from rfoundry_hil import skip


@pytest.mark.feature("p25_zone_scan")
def test_p25_zone_scan_view(radio):
    if "P25_ZONE_SCAN" not in radio.features():
        skip("not_built", "built without CONFIG_P25_ZONE_SCAN")
    view = radio.view(["p25_trunk"])
    assert view.get("p25t.zone_scan") in ("0", "1")
    count = int(view["p25t.scan_groups"])
    assert 0 <= count <= 80
    if view["p25t.zone_scan"] == "1":
        assert count >= 1
        assert view["p25t.tx"] == "0"
    else:
        assert count == 0
