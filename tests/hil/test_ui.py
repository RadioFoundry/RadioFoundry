# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The UI driven by injected keys over the debug link (docs/spec/ui.md): channel up / down on Home, the Hub and its
destinations, Quick settings, type-to-find, the key lock, long BACK = Home from anywhere, the diagnostics page,
screenshots that change with the screen.

The checks read the UI's state from the link section (ui.screen, ui.depth, ui.focus, ui.locked, ui.query, ui.toast)
rather than from pixels, so they hold on any display; the screenshots are kept for a person to look at. The same
key sequences run on the host in tests/host/uiFlowTest.c (every display class and key map), so a failure here that
the host test doesn't show points at the radio side (input, link, display), not the UI logic."""
import pytest

from odr_hil import LONG


def diff_fraction(a, b):
    n = min(len(a), len(b)) // 2
    return sum(1 for i in range(0, n * 2, 2) if a[i:i + 2] != b[i:i + 2]) / max(1, n)


def ui(r):
    return r.view(["link"])


def wait_screen(r, name, what=None):
    return r.wait_for(lambda v: v.get("ui.screen") == name, sections=("link",), what=what or f"screen {name}")


@pytest.fixture
def home(radio):
    radio.ensure_awake()
    if ui(radio).get("ui.locked") == "1":
        radio.press("quick", LONG)
        radio.wait_for(lambda v: v.get("ui.locked") == "0", sections=("link",), what="keys unlocked")
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    if radio.screen() != "home":
        radio.press("back", LONG)
    wait_screen(radio, "home", "Home")
    if radio.core().get("vfo") == "1":
        radio.link.vfo(False)
        radio.wait_for(lambda v: v.get("vfo") == "0", what="VFO off")
    yield radio
    # Leave the radio on Home with the keys unlocked whatever the test did
    if ui(radio).get("ui.locked") == "1":
        radio.press("quick", LONG)
    radio.press("back", LONG)


def test_keys_move_the_channel(home):
    r = home
    v = r.core()
    if int(v["zone_size"]) < 2:
        pytest.skip("the zone has one channel")
    pos = int(v["zone_pos"])
    r.press("up")
    v1 = r.wait_for(lambda x: x["zone_pos"] != str(pos), what="channel change on UP")
    r.press("down")
    r.wait_for(lambda x: x["zone_pos"] == str(pos), what="back on DOWN")
    assert v1["channel_number"] != v["channel_number"] or v1["rx_hz"] != v["rx_hz"]


def test_hub_and_back(home):
    """OK opens the Hub (six destinations, numbered); a digit opens one; BACK goes up one level; long BACK goes Home
    from any depth (information-architecture.md, key meanings)."""
    r = home
    r.press("ok")
    s = wait_screen(r, "hub", "the Hub on OK")
    assert s["ui.depth"] == "1" and s["ui.count"] == "6", s
    r.press("1")
    s = wait_screen(r, "contacts", "Contacts on 1")
    assert s["ui.depth"] == "2"
    r.press("back")
    wait_screen(r, "hub", "BACK goes up to the Hub")
    r.press("6")
    wait_screen(r, "settings", "Settings on 6")
    r.press("2")
    wait_screen(r, "group", "a settings group on 2")
    r.press("back", LONG)
    s = wait_screen(r, "home", "long BACK goes Home from depth 3")
    assert s["ui.depth"] == "0"


def test_quick_settings_open_and_close(home, shots):
    """QUICK opens the tile grid, a second QUICK or BACK closes it. Screenshots go through Radio.shot_until: a capture
    freezes the UI's redraws for a moment, so one taken right after a key can still show the old screen."""
    r = home
    _, before = r.shot_until(lambda d: True, what="Home")
    r.press("quick")
    s = wait_screen(r, "quick", "Quick settings open")
    assert s["ui.depth"] == "1"
    info, opened = r.shot_until(lambda d: diff_fraction(before, d) > 0.10, what="Quick settings drawn")
    r.save_screenshot(f"{shots}/ui_quick.png", data=opened, info=info)
    r.press("back")
    wait_screen(r, "home", "BACK closes Quick settings")
    r.press("quick")
    wait_screen(r, "quick", "Quick settings again")
    r.press("quick")
    wait_screen(r, "home", "QUICK closes Quick settings")


def test_find_by_digits(home, shots):
    """Digits on Home start type-to-find (task-flows.md: find a contact by name with the keypad letters)."""
    r = home
    for k in "563":
        r.press(k)
    s = wait_screen(r, "find", "Find on a digit")
    assert s["ui.query"] == "563", s
    info, data = r.shot_until(lambda d: True, what="Find")
    r.save_screenshot(f"{shots}/ui_find.png", data=data, info=info)
    r.press("back")
    r.wait_for(lambda v: v.get("ui.screen") in ("home", "find") and v.get("ui.query", "") != "563",
               sections=("link",), what="BACK edits the query or leaves")
    r.press("back", LONG)
    wait_screen(r, "home")


def test_key_lock(home):
    """Long QUICK locks the keys; while locked, a key only explains how to unlock; long QUICK unlocks."""
    r = home
    r.press("quick", LONG)
    r.wait_for(lambda v: v.get("ui.locked") == "1", sections=("link",), what="keys locked")
    r.press("ok")
    s = ui(r)
    assert s["ui.screen"] == "home", "a locked radio opened a screen"
    assert "unlock" in s.get("ui.toast", ""), s
    r.press("quick", LONG)
    r.wait_for(lambda v: v.get("ui.locked") == "0", sections=("link",), what="keys unlocked")


def test_recent_on_long_context(home):
    r = home
    r.press("context", LONG)
    wait_screen(r, "recent", "Recent on long #")
    r.press("back")
    wait_screen(r, "home")


def test_diagnostics_page(home):
    """Diagnostics live in Tools (Hub 5, then 7); long BACK on Home no longer opens them."""
    r = home
    r.press("ok")
    wait_screen(r, "hub")
    r.press("5")
    wait_screen(r, "tools", "Tools on 5")
    r.press("7")
    s = r.wait_for(lambda v: v.get("ui.screen") in ("diag", "tools"), sections=("link",), what="diagnostics")
    if s["ui.screen"] != "diag":
        pytest.skip("built without CONFIG_DIAG")
    r.press("back")
    wait_screen(r, "tools", "BACK leaves the diagnostics")
    r.press("back", LONG)
    wait_screen(r, "home")


def test_screenshot_sanity(home):
    info, data = home.shot_until(lambda d: True, what="a picture")
    px = {data[i:i + 2] for i in range(0, len(data), 2)} if info["bpp"] == 16 else set(data)
    assert len(px) > 2, "the screen is blank"


def test_injected_keys_count_as_activity(home):
    r = home
    r.press("quick")
    s = ui(r)
    assert int(s["input.idle_ms"]) < 2000 and s["input.source"] == "host"
    r.press("back")
