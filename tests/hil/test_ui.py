# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The UI driven by injected keys: channel up / down on Home, Quick settings open and close, the diagnostics page,
screenshots that change with the screen."""
import pytest

from odr_hil import LONG


def diff_fraction(a, b):
    n = min(len(a), len(b)) // 2
    return sum(1 for i in range(0, n * 2, 2) if a[i:i + 2] != b[i:i + 2]) / max(1, n)


@pytest.fixture
def home(radio):
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    return radio


def test_keys_move_the_channel(home):
    r = home
    if r.core().get("vfo") == "1":
        r.link.vfo(False)
        r.wait_for(lambda v: v.get("vfo") == "0", what="VFO off")
    v = r.core()
    if int(v["zone_size"]) < 2:
        pytest.skip("the zone has one channel")
    pos = int(v["zone_pos"])
    r.press("up")
    v1 = r.wait_for(lambda x: x["zone_pos"] != str(pos), what="channel change on UP")
    r.press("down")
    r.wait_for(lambda x: x["zone_pos"] == str(pos), what="back on DOWN")
    assert v1["channel_number"] != v["channel_number"] or v1["rx_hz"] != v["rx_hz"]


def test_quick_settings_open_and_close(home, shots):
    """Screenshots go through Radio.shot_until: a capture freezes the UI's redraws for a moment, so one taken right
    after a key can still show the old screen (or a redraw half done); shot_until waits the hold out and retries
    until the picture is what the test waits for."""
    r = home
    _, before = r.shot_until(lambda d: True, what="Home")
    r.press("quick")
    r.wait_for(lambda v: v.get("ui.screen") == "quick", sections=("link",), what="Quick settings open")
    info, opened = r.shot_until(lambda d: diff_fraction(before, d) > 0.10, what="Quick settings drawn")
    r.save_screenshot(f"{shots}/quick.png", data=opened, info=info)
    r.press("back")
    r.wait_for(lambda v: v.get("ui.screen") == "home", sections=("link",), what="back to Home")
    r.shot_until(lambda d: diff_fraction(before, d) < 0.05, what="Home as before Quick settings")


def test_diagnostics_page(home):
    r = home
    r.press("back", LONG)
    r.wait_for(lambda v: v.get("ui.screen") == "diag", sections=("link",), what="diagnostics page")
    r.press("back")
    r.wait_for(lambda v: v.get("ui.screen") == "home", sections=("link",), what="back to Home")


def test_screenshot_sanity(home):
    info, data = home.shot_until(lambda d: True, what="a picture")
    px = {data[i:i + 2] for i in range(0, len(data), 2)} if info["bpp"] == 16 else set(data)
    assert len(px) > 2, "the screen is blank"


def test_injected_keys_count_as_activity(home):
    r = home
    r.press("quick")
    idle = int(r.view(["link"])["input.idle_ms"])
    assert idle < 2000 and r.view(["link"])["input.source"] == "host"
    r.press("back")
