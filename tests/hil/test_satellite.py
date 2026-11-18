# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Satellite tracking observability through the debug-link core view."""
import pytest

from rfoundry_hil import as_int, skip


@pytest.mark.feature(
    "satellite_tle",
    optional=True,
    owner="load a current TLE, provide position and UTC, then start tracking on Tools > Satellite",
)
def test_satellite_tracking_view(radio):
    view = radio.view(["core"])
    if "satellite.tracking" not in view:
        skip("not_built", "firmware without CONFIG_SATELLITE_TLE")
    if as_int(view["satellite.tracking"]) != 1:
        skip("state", "start satellite tracking with a valid TLE, position and UTC")
    assert 0 <= as_int(view["satellite.index"]) < 10, view
    assert as_int(view["satellite.status"]) in (1, 2, 3), view
