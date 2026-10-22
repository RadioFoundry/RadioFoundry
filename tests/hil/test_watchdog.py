# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The hardware watchdog (docs/spec/watchdog.md): the view section, the longest idle gap, and the bench tests that stop
the feeds on purpose and must end in a reset. Those restart the radio: opt-in with --allow-reboot (marker reboot)."""
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, as_int, find_ports, skip


def _view(radio):
    v = radio.view(["watchdog"])
    if v.get("watchdog.built") != "1":
        skip("not_built", "built without CONFIG_WATCHDOG")
    return v


def _reconnect(radio, timeout=60.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for port in [radio.port] + find_ports():
            try:
                radio.link = DebugLink(port, timeout=1.5)
                radio.link.ping()
                radio.port = port
                return
            except (DebugLinkError, OSError):
                time.sleep(0.5)
    raise AssertionError("the radio did not come back after the watchdog reset")


def test_watchdog_running(radio):
    """Started and fed: a timeout, the feed count going up, and the idle task's longest gap well inside the timeout."""
    v = _view(radio)
    timeout = as_int(v["watchdog.timeout_ms"])
    assert 1000 <= timeout <= 32000, v
    assert v.get("watchdog.starving") == "0", v
    feeds = as_int(v["watchdog.feeds"])
    time.sleep(0.5)
    v2 = _view(radio)
    assert as_int(v2["watchdog.feeds"]) > feeds, (v, v2)
    assert as_int(v2["watchdog.max_gap_ms"]) < timeout // 2, v2


def test_watchdog_test_refused_without_token(radio):
    _view(radio)
    with pytest.raises(DebugLinkError) as e:
        radio.link.request(0x1D, bytes([1, 1, 0, 0, 0, 0]))
    assert e.value.status == 7     # refused: no starving the watchdog without the token


@pytest.mark.reboot
@pytest.mark.parametrize("kind", ["starve", "spin", "irq-off"])
def test_watchdog_resets(radio, kind):
    """Stop the feeds three ways: the radio must reset within the timeout (plus the restart) and come back fed."""
    timeout_s = as_int(_view(radio)["watchdog.timeout_ms"]) / 1000.0
    up_before = as_int(radio.view(["link"])["uptime_ms"])
    t0 = time.monotonic()
    radio.link.watchdog_test(kind)
    radio.link.close()
    time.sleep(timeout_s)
    _reconnect(radio, timeout=timeout_s + 30.0)
    back_s = time.monotonic() - t0
    up_after = as_int(radio.view(["link"])["uptime_ms"])
    assert up_after < up_before, "the radio didn't restart"
    # the reset came within the timeout: what the radio has been up for plus the timeout covers the time since the test
    assert back_s <= timeout_s + up_after / 1000.0 + 3.0, (back_s, timeout_s, up_after)
    v = _view(radio)
    assert v.get("watchdog.starving") == "0", v
