# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Satellite tracking observability through the debug-link core view."""
import pytest

from rfoundry_hil import as_int, skip
from test_task_stacks import HEAP_MIN_FREE, tasks_section


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


@pytest.mark.feature(
    "satellite_tle",
    optional=True,
    owner="load a current TLE, provide position and UTC, then start tracking on Tools > Satellite",
)
def test_satellite_tracking_heap(radio):
    view = radio.view(["core"])
    if "satellite.tracking" not in view:
        skip("not_built", "firmware without CONFIG_SATELLITE_TLE")
    if as_int(view["satellite.tracking"]) != 1:
        skip("state", "start satellite tracking with a valid TLE, position and UTC")
    tasks = tasks_section(radio)
    if tasks is None or tasks.get("tasks.built") == "0" or "tasks.heap_min_free" not in tasks:
        skip("not_built", "firmware without task heap reporting")
    least = as_int(tasks["tasks.heap_min_free"])
    assert least >= HEAP_MIN_FREE, f"RTOS heap down to {least} bytes free (want {HEAP_MIN_FREE})"
