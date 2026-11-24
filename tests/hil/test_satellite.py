# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Satellite tracking observability through the debug-link core view."""
import time

import pytest

from rfoundry_hil import LONG, as_int, skip
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


@pytest.mark.feature(
    "satellite_tle",
    optional=True,
    owner="load current TLEs and profiles, provide position and UTC, and leave the radio idle on Home",
)
def test_satellite_exit_restores_channel(radio):
    radio.ensure_awake()
    radio.unlock()
    if "SATELLITE_TLE" not in radio.features():
        skip("not_built", "firmware without CONFIG_SATELLITE_TLE")
    if radio.screen() != "home":
        skip("state", "leave the radio on Home before the test")
    before = radio.view(["core", "channel"])
    if before.get("state") != "idle" or before.get("satellite.tracking") != "0":
        skip("state", "the radio must be idle and not tracking")
    keys = ("vfo", "vfo_index", "zone_index", "zone_pos", "channel_number", "channel_name",
            "mode", "rx_hz", "tx_hz", "ch.bw", "ch.rx_tone", "ch.tx_tone")
    try:
        for key in ("ok", "6", "9"):
            radio.press(key)
        radio.wait_for(lambda v: v.get("ui.screen") == "satellite-passes", sections=("link",),
                       what="satellite pass list")
        end = time.monotonic() + 120
        while time.monotonic() < end:
            if as_int(radio.view(["link"]).get("ui.focus"), 0) > 0:
                break
            time.sleep(0.5)
        else:
            skip("state", "no predicted satellite pass is available")
        radio.press("ok")
        radio.wait_for(lambda v: v.get("ui.screen") == "satellite", sections=("link",),
                       what="satellite detail")
        radio.wait_for(lambda v: v.get("satellite.tracking") == "1", what="satellite tracking")
        radio.press("back", LONG)
        after = radio.wait_for(lambda v: v.get("satellite.tracking") == "0" and
                               all(v.get(k) == before.get(k) for k in keys),
                               sections=("core", "channel"), what="restored channel or VFO")
        assert all(after.get(k) == before.get(k) for k in keys), after
    finally:
        radio.press("back", LONG)
