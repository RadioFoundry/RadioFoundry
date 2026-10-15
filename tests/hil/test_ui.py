# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The UI driven by injected keys over the debug link (docs/spec/ui.md): channel up / down on Home, the Hub and its
destinations, Quick settings, type-to-find, the key lock, long BACK = Home from anywhere, the diagnostics page,
screenshots that change with the screen.

The checks read the UI's state from the link section (ui.screen, ui.depth, ui.focus, ui.locked, ui.query, ui.toast)
rather than from pixels, so they hold on any display; the screenshots are kept for a person to look at. The same
key sequences run on the host in tests/host/uiFlowTest.c (every display class and key map), so a failure here that
the host test doesn't show points at the radio side (input, link, display), not the UI logic."""
import os
import time

import pytest

from odr_hil import LONG, as_int
from odr_debug import DOWN, UP


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
    """OK opens the Hub (seven destinations, numbered); a digit opens one; BACK goes up one level; long BACK goes Home
    from any depth (information-architecture.md, key meanings)."""
    r = home
    r.press("ok")
    s = wait_screen(r, "hub", "the Hub on OK")
    assert s["ui.depth"] == "1" and s["ui.count"] == "7", s
    r.press("1")
    s = wait_screen(r, "contacts", "Contacts on 1")
    assert s["ui.depth"] == "2"
    r.press("back")
    wait_screen(r, "hub", "BACK goes up to the Hub")
    r.press("7")
    wait_screen(r, "settings", "Settings on 7")
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


def test_talk_target_picker_on_long_context(home):
    """Long # opens the talk target picker on a DMR channel (Recent is Hub 3)."""
    r = home
    if r.core().get("mode") != "dmr":
        pytest.skip("the channel isn't DMR")
    r.press("context", LONG)
    wait_screen(r, "picker", "talk target picker on long #")
    r.press("back")
    wait_screen(r, "home")


def test_recent_from_the_hub(home):
    r = home
    r.press("ok")
    wait_screen(r, "hub")
    r.press("3")
    wait_screen(r, "recent", "Recent on 3")
    r.press("back", LONG)
    wait_screen(r, "home")


def test_zones_and_channels_lists(home):
    """Hub 4 lists the VFO and the zones, hub 5 every channel by number; OK on a channel opens its settings page."""
    r = home
    r.press("ok")
    wait_screen(r, "hub")
    r.press("4")
    s = wait_screen(r, "zones", "Zones on 4")
    assert as_int(s.get("ui.count")) >= 2, s			# the VFO and at least one zone
    r.press("back")
    wait_screen(r, "hub")
    r.press("5")
    s = wait_screen(r, "channels", "Channels on 5")
    if as_int(s.get("ui.count")) == 0:
        pytest.skip("no channels in the codeplug")
    r.press("ok")
    wait_screen(r, "channel", "a channel's settings on OK")
    r.press("back", LONG)
    wait_screen(r, "home")


def test_diagnostics_page(home):
    """Diagnostics live in Tools (Hub 6, then 7); long BACK on Home no longer opens them."""
    r = home
    r.press("ok")
    wait_screen(r, "hub")
    r.press("6")
    wait_screen(r, "tools", "Tools on 6")
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


# ---------------------------------------------------------------------------------------------------------------------
# The controls that were "not in this build" stubs before the glue package (ui.md section 4): each one now reaches the
# radio core, the data service or the store. The Quick digits are the keypad positions of the tiles: on a DMR channel
# 2 slot, 3 color code, 4 mode, 5 hear all; on an FM channel 5 monitor, 6 VOX (ui_quick_kind). The same sequences run
# on the host in uiFlowTest.c.


def dmr_home(r):
    if r.core().get("mode") != "dmr":
        pytest.skip("the current channel isn't DMR")
    return r


def fm_home(r):
    if r.core().get("mode") != "fm":
        pytest.skip("the current channel isn't FM")
    return r


def temp_bits(v):
    return as_int(v.get("temp"), 0)


def leave_quick(r):
    if r.screen() == "quick":
        r.press("back")
    wait_screen(r, "home")


def test_quick_timeslot_for_now(home):
    """Quick 2 flips the timeslot for now (temp bit 0x20); BACK on Home undoes it."""
    r = dmr_home(home)
    ts = as_int(r.view(["dmr"]).get("dmr.ts"), 1)
    r.press("quick")
    wait_screen(r, "quick")
    r.press("2")
    v = r.wait_for(lambda x: as_int(x.get("dmr.ts")) == (1 if ts == 2 else 2) and temp_bits(x) & 0x20,
                   sections=("core", "dmr"), what="the other timeslot, marked temporary")
    assert v["temporary"] == "1", v
    leave_quick(r)
    r.press("back")
    r.wait_for(lambda x: as_int(x.get("dmr.ts")) == ts and not temp_bits(x) & 0x20, sections=("core", "dmr"),
               what="the channel's timeslot back on BACK")


def test_quick_color_code_for_now(home):
    """Quick 3, a digit, OK: the color code for now (temp bit 0x40)."""
    r = dmr_home(home)
    cc = as_int(r.view(["dmr"]).get("dmr.cc"), 1)
    new = 7 if cc != 7 else 5
    r.press("quick")
    wait_screen(r, "quick")
    r.press("3")
    r.press(str(new))
    r.press("ok")
    r.wait_for(lambda x: as_int(x.get("dmr.cc")) == new and temp_bits(x) & 0x40, sections=("core", "dmr"),
               what=f"color code {new}, marked temporary")
    leave_quick(r)
    r.press("back")
    r.wait_for(lambda x: as_int(x.get("dmr.cc")) == cc and not temp_bits(x) & 0x40, sections=("core", "dmr"),
               what="the channel's color code back")


def test_quick_mode_for_now(home):
    """Quick 4 on a DMR channel listens in FM on the same frequency for now (temp bit 0x80)."""
    r = dmr_home(home)
    r.press("quick")
    wait_screen(r, "quick")
    r.press("4")
    r.wait_for(lambda x: x.get("mode") == "fm" and temp_bits(x) & 0x80, what="FM for now")
    leave_quick(r)
    r.press("back")
    r.wait_for(lambda x: x.get("mode") == "dmr" and not temp_bits(x) & 0x80, what="DMR again on BACK")


def test_quick_hear_all_latch(home):
    """Quick 5 latches hear all (every talkgroup on the slot); Quick 5 again or BACK on Home drops it."""
    r = dmr_home(home)
    r.press("quick")
    wait_screen(r, "quick")
    r.press("5")
    r.wait_for(lambda x: x.get("dmr.hear_all_latch") == "1" and x.get("dmr.hear_all") == "1", sections=("dmr",),
               what="hear all latched")
    leave_quick(r)
    r.press("back")
    r.wait_for(lambda x: x.get("dmr.hear_all_latch") == "0", sections=("dmr",), what="hear all off on BACK")


def test_quick_monitor_latch(home):
    """Quick 5 on an FM channel latches the monitor (squelch open) until Quick 5 again."""
    r = fm_home(home)
    r.press("quick")
    wait_screen(r, "quick")
    r.press("5")
    r.wait_for(lambda x: x.get("monitor") == "1", what="monitor latched")
    r.press("5")
    r.wait_for(lambda x: x.get("monitor") == "0", what="monitor off again")
    leave_quick(r)


def test_quick_vox_toggle(home):
    """Quick 6 on an FM channel turns VOX on (setting vox = all) and off again."""
    r = fm_home(home)
    was = r.view(["features"]).get("vox_on")
    if was is None:
        pytest.skip("firmware without the vox_on key")
    r.press("quick")
    wait_screen(r, "quick")
    r.press("6")
    v = r.view(["features", "link"])
    if v.get("vox_on") == was and "build" in v.get("ui.toast", ""):
        leave_quick(r)
        pytest.skip("built without CONFIG_VOX")
    want = "0" if was == "1" else "1"
    r.wait_for(lambda x: x.get("vox_on") == want, sections=("features",), what=f"vox_on={want}")
    r.press("6")
    r.wait_for(lambda x: x.get("vox_on") == was, sections=("features",), what="VOX as it was")
    leave_quick(r)


def test_picker_focus_target(home):
    """The talk target picker tells the radio core which row has the focus (the one-shot PTT target); long BACK
    clears it. Nothing transmits: the one-shot only arms on PTT (radio_tx tests key it)."""
    r = dmr_home(home)
    r.press("tt")
    s = wait_screen(r, "picker", "the talk target picker")
    if as_int(s.get("ui.count"), 0) < 2:
        r.press("back", LONG)
        pytest.skip("the picker has no talk target rows")
    # The focus starts on the first target; the last row ("Type a number...") has none
    v = r.wait_for(lambda x: as_int(x.get("dmr.focus_id"), 0) != 0, sections=("dmr",), what="a focus id")
    assert v["dmr.one_shot"] == "0", v
    # Down walks the rows: rows without a talk target (text rows, "Type a number...") clear the focus, the next
    # target row sets its own; the walk wraps around, so a second target is found if there is one
    first = v["dmr.focus_id"]
    others = set()
    for _ in range(as_int(s.get("ui.count"), 0)):
        r.press("down")
        time.sleep(0.15)
        f = r.view(["dmr"]).get("dmr.focus_id")
        if f not in (first, "0", None):
            others.add(f)
            break
    if as_int(s.get("ui.count"), 0) >= 3 and not others:
        print("picker: only one talk target row")
    r.press("back", LONG)
    wait_screen(r, "home")
    r.wait_for(lambda x: as_int(x.get("dmr.focus_id"), 0) == 0, sections=("dmr",), what="the focus cleared")


def settings_keys(r, keys):
    """Run a key sequence from Home (Settings is Hub 7; groups 1-9 then 0), then go Home."""
    for k in keys.split():
        r.press(*((k[5:], LONG) if k.startswith("long:") else (k,)))
    r.press("back", LONG)
    wait_screen(r, "home")


def test_long_press_time_reaches_input(home):
    """Settings > Keys > Long press time: the gesture engine uses the new time at once (input.long_ms)."""
    r = home
    before = as_int(r.view(["link"]).get("input.long_ms"))
    if before is None:
        pytest.skip("firmware without input.long_ms")
    if before >= 1200:
        pytest.skip("the long press time is already at its maximum")
    settings_keys(r, "ok 7 8 ok up ok")
    try:
        r.wait_for(lambda x: as_int(x.get("input.long_ms")) == before + 100, sections=("link",),
                   what="long press time +100 ms")
        s = r.view(["storage"])
        assert as_int(s.get("settings.long_press_ms")) == before + 100, s
    finally:
        settings_keys(r, "ok 7 8 ok down ok")
    r.wait_for(lambda x: as_int(x.get("input.long_ms")) == before, sections=("link",), what="long press time back")


def test_auto_lock(home):
    """Settings > Privacy > Auto lock 30 s locks the keys after 30 s without a key; set back to Off after."""
    r = home
    if as_int(r.view(["storage"]).get("settings.auto_lock_s"), 0) != 0:
        pytest.skip("auto lock is already on")
    settings_keys(r, "ok 7 4 down down ok")
    try:
        r.wait_for(lambda x: x.get("settings.auto_lock_s") == "30", sections=("storage",), what="auto lock 30 s")
        assert ui(r)["ui.locked"] == "0"
        r.wait_for(lambda x: x.get("ui.locked") == "1", timeout=40.0, interval=1.0, sections=("link",),
                   what="locked after 30 s idle")
    finally:
        if ui(r).get("ui.locked") == "1":
            r.press("quick", LONG)
            r.wait_for(lambda x: x.get("ui.locked") == "0", sections=("link",), what="keys unlocked")
        r.press("ok")
        r.press("7")
        r.press("4")
        r.press("down")
        r.press("down")
        for _ in range(4):
            if r.view(["storage"]).get("settings.auto_lock_s") == "0":
                break
            r.press("ok")
        r.press("back", LONG)
    r.wait_for(lambda x: x.get("settings.auto_lock_s") == "0", sections=("storage",), what="auto lock off again")


def storage(r):
    return r.view(["storage"])


def ui_flags(r):
    return as_int(storage(r).get("settings.ui_flags"))


def test_theme_is_kept(home):
    """Settings > Display > Theme: Light is stored (settings.theme) and the screen changes; back to Dark after."""
    r = home
    s = storage(r)
    if "settings.theme" not in s:
        pytest.skip("firmware without settings.theme")
    if s["settings.theme"] != "0":
        pytest.skip("the theme isn't Dark")
    if r.version().get("display", "").endswith("x1"):
        pytest.skip("a monochrome screen has no themes (the radio says so)")
    _, before = r.shot_until(lambda d: True, what="Home")
    settings_keys(r, "ok 7 2 down down ok")
    try:
        r.wait_for(lambda x: x.get("settings.theme") == "1", sections=("storage",), what="theme Light stored")
        r.shot_until(lambda d: diff_fraction(before, d) > 0.2, what="Home in the light theme")
    finally:
        settings_keys(r, "ok 7 2 down down ok ok")
    r.wait_for(lambda x: x.get("settings.theme") == "0", sections=("storage",), what="theme Dark again")


def test_updown_changes_the_zone(home):
    """Settings > Keys > Up/Down on Home = Zone: Up on Home moves to the next zone, not the next channel."""
    r = home
    s = storage(r)
    if s.get("settings.updown_home") != "0":
        pytest.skip("firmware without settings.updown_home, or it isn't Channel")
    if as_int(r.core().get("zone_count"), 2) < 2:
        pytest.skip("only one zone")
    zone = as_int(r.core().get("zone_index"))
    settings_keys(r, "ok 7 8 down down ok")
    try:
        r.wait_for(lambda x: x.get("settings.updown_home") == "1", sections=("storage",), what="Up/Down = Zone")
        r.press("up")
        r.wait_for(lambda x: as_int(x.get("zone_index")) != zone, what="zone change on UP")
        r.press("down")
        r.wait_for(lambda x: as_int(x.get("zone_index")) == zone, what="the zone back on DOWN")
    finally:
        settings_keys(r, "ok 7 8 down down ok ok")
    r.wait_for(lambda x: x.get("settings.updown_home") == "0", sections=("storage",), what="Up/Down = Channel again")


def test_privacy_key_entry(home):
    """Settings > Privacy: key type Motorola, key typed 123 (slot 1); type Off again removes it."""
    r = home
    s = storage(r)
    if "settings.privacy_type" not in s:
        pytest.skip("firmware without settings.privacy_type")
    if s["settings.privacy_type"] != "0":
        pytest.skip("a privacy key is set already: not overwriting it")
    settings_keys(r, "ok 7 4 ok down ok 1 2 3 ok")
    try:
        r.wait_for(lambda x: x.get("settings.privacy_type") == "1", sections=("storage",), what="key type Motorola")
        if r.core().get("mode") == "dmr":
            r.press("quick")
            wait_screen(r, "quick")
            r.press("6")
            r.wait_for(lambda x: "Privacy on" in x.get("ui.toast", ""), sections=("link",), what="privacy on")
            leave_quick(r)
            r.press("back")
    finally:
        settings_keys(r, "ok 7 4 ok ok")	# Motorola -> Anytone -> Off
    r.wait_for(lambda x: x.get("settings.privacy_type") == "0", sections=("storage",), what="the key removed")


@pytest.mark.radio_tx
def test_ptt_when_locked_off(home):
    """Settings > Privacy > PTT when locked Off: with the keys locked an injected PTT doesn't transmit and the UI
    says how to unlock. Marked radio_tx: if the block failed the radio would key up (lowest power)."""
    r = home
    flags = ui_flags(r)
    if flags is None:
        pytest.skip("firmware without settings.ui_flags")
    if not flags & 0x10:
        pytest.skip("PTT when locked is already off")
    r.lowest_power()
    settings_keys(r, "ok 7 4 down down down ok")
    try:
        r.wait_for(lambda x: not as_int(x.get("settings.ui_flags"), 0x10) & 0x10, sections=("storage",),
                   what="PTT when locked off")
        r.press("quick", LONG)
        r.wait_for(lambda x: x.get("ui.locked") == "1", sections=("link",), what="keys locked")
        r.link.key("ptt", DOWN)
        try:
            time.sleep(0.8)
            v = r.view(["core", "link"])
            assert v.get("state") != "tx", v
            assert "PTT is off" in v.get("ui.toast", ""), v
        finally:
            r.link.key("ptt", UP)
        r.press("quick", LONG)
        r.wait_for(lambda x: x.get("ui.locked") == "0", sections=("link",), what="keys unlocked")
    finally:
        if ui(r).get("ui.locked") == "1":
            r.press("quick", LONG)
        settings_keys(r, "ok 7 4 down down down ok")
    r.wait_for(lambda x: as_int(x.get("settings.ui_flags"), 0) & 0x10, sections=("storage",),
               what="PTT when locked on again")


def test_usb_network_switch_row(home):
    """Tools > USB network: the info page has the one "Switch USB to network" row. The test backs out without
    switching (switching drops the serial link; see the HIL steps in docs/user/14-usb-network.md)."""
    r = home
    r.press("ok")
    wait_screen(r, "hub")
    r.press("6")
    wait_screen(r, "tools")
    r.press("4")
    v = r.wait_for(lambda x: x.get("ui.screen") == "info", sections=("link",), what="the USB network page")
    if v.get("ui.count") != "1":
        pytest.skip("built without CONFIG_USB_NETWORK")
    assert v.get("ui.focus") == "0", v
    r.press("back", LONG)
    wait_screen(r, "home")


@pytest.mark.codeplug_write
def test_save_timeslot_to_channel(home):
    """Quick 2 then hold OK saves the timeslot into the channel off the UI task (storage.job_busy), the temporary mark
    goes, and the channel keeps the new slot; the test saves the old slot back the same way."""
    r = dmr_home(home)
    ts = as_int(r.view(["dmr"]).get("dmr.ts"), 1)
    other = 1 if ts == 2 else 2

    def save_slot(want):
        r.press("quick")
        wait_screen(r, "quick")
        r.press("2")
        r.wait_for(lambda x: as_int(x.get("dmr.ts")) == want, sections=("dmr",), what=f"timeslot {want}")
        r.press("ok", LONG)
        r.wait_for(lambda x: x.get("storage.job_busy") == "0" and x.get("temporary") == "0" and
                   as_int(x.get("dmr.ts")) == want, timeout=5.0, sections=("core", "dmr", "storage"),
                   what=f"timeslot {want} saved, nothing temporary")
        assert "Couldn't" not in ui(r).get("ui.toast", "")
        leave_quick(r)

    save_slot(other)
    save_slot(ts)


@pytest.mark.skipif(os.environ.get("ODR_HIL_DELETE_MESSAGE") != "1",
                    reason="deletes the newest inbox message: set ODR_HIL_DELETE_MESSAGE=1")
def test_delete_message(home):
    """Hub 2 (Messages), OK on the newest, Delete (the fourth action): the inbox has one message less."""
    r = home
    n = as_int(r.view(["dmr_data"]).get("data.inbox_count"))
    if not n:
        pytest.skip("the inbox is empty (or no data core)")
    r.press("ok")
    wait_screen(r, "hub")
    r.press("2")
    wait_screen(r, "messages")
    r.press("ok")
    wait_screen(r, "message")
    for _ in range(3):
        r.press("down")
    r.press("ok")
    r.wait_for(lambda x: as_int(x.get("data.inbox_count")) == n - 1, sections=("dmr_data",), what="one message less")
    assert "deleted" in ui(r).get("ui.toast", "")


@pytest.mark.reboot
@pytest.mark.skipif(os.environ.get("ODR_HIL_FACTORY_RESET") != "1",
                    reason="erases the settings and the native area: set ODR_HIL_FACTORY_RESET=1 (and --allow-reboot)")
def test_factory_reset(home):
    """Settings > Advanced (0) > Factory reset > Yes: the settings go back to their defaults, the native area is erased
    (never the codeplug or calibration), and the radio restarts. The channel position survives only as the default."""
    from test_cps import reconnect
    r = home
    zone_size = r.core().get("zone_size")
    for k in ("ok", "6", "0", "down", "down", "down", "ok"):
        r.press(k)
    wait_screen(r, "confirm", "the confirmation (defaults to No)")
    assert ui(r)["ui.focus"] == "0"
    r.press("down")
    r.press("ok", settle=0)
    time.sleep(2.0)
    reconnect(r)
    s = r.view(["storage", "link"])
    assert as_int(s.get("settings.long_press_ms")) in (0, 600) and s.get("settings.auto_lock_s") == "0", s
    assert as_int(s.get("settings.failures"), 0) == 0, s
    assert r.core().get("zone_size") == zone_size, "the codeplug changed"
