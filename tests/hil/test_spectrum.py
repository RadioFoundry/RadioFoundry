# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Receiver sweep checks; manual capture needs an external RF source and --allow-rf."""
import time
import pytest

from rfoundry_hil import LONG, skip


def _numeric(radio, value):
    radio.press("ok")
    for digit in str(value):
        radio.press(digit)
    radio.press("ok")


def _auto_mode(radio, mode):
    for _ in range(3):
        if radio.view(["channel"]).get("spectrum.auto_peak_mode") == mode:
            return
        radio.press("ok")
    radio.wait_for(lambda v: v.get("spectrum.auto_peak_mode") == mode, sections=("channel",),
                   what=f"automatic peak mode {mode}")


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
def test_spectrum_tuner_edit_and_restore(radio):
    radio.ensure_awake()
    radio.unlock()
    radio.press("back", LONG)
    before = radio.view(["channel"])
    if before.get("ch.mode") not in ("fm", "dmr"):
        skip("state", "select a conventional RF channel before this test")
    if before.get("ch.sfr") != "0,0":
        skip("state", "select a channel without an SFR role")
    try:
        radio.press("ok")
        radio.press("6")
        radio.press("0")
        active = radio.wait_for(lambda v: v.get("spectrum.active") == "1", sections=("channel",),
                                what="spectrum receiver ownership")
        assert active["spectrum.bins"] in ("65", "97", "129")
        center = int(active["spectrum.center_hz"])
        tuner = int(active["spectrum.tuner_hz"])
        step = int(active["spectrum.step_hz"])
        at_end = tuner >= center + int(active["spectrum.bins"]) // 2 * step
        radio.press("left" if at_end else "right")
        target = tuner - step if at_end else tuner + step
        moved = radio.wait_for(lambda v: int(v.get("spectrum.tuner_hz", "0")) == target,
                               sections=("channel",), what="independent listening tuner")
        if active["spectrum.center_tuner"] == "1":
            center = target
        assert int(moved["spectrum.center_hz"]) == center
        radio.press("ok")
        menu = radio.wait_for(lambda v: v.get("ui.screen") == "spectrum-settings",
                              sections=("link", "channel"), what="spectrum settings menu")
        assert menu["spectrum.active"] == "1"
        _numeric(radio, 0)
        value = radio.wait_for(lambda v: v.get("spectrum.squelch") == "0", sections=("channel",),
                               what="numeric squelch entry")
        assert value["spectrum.threshold_dbm"] == "0"
        sweeps = value["spectrum.sweeps"]
        # Scan progress assumes a quiet bench at the maximum squelch setting.
        radio.wait_for(lambda v: v.get("spectrum.sweeps") != sweeps, timeout=180, sections=("channel",),
                       what="sweep completion in settings")
        radio.press("down")
        radio.press("down")
        expected = int(value["spectrum.bins"])
        for _ in range(3):
            expected = {65: 97, 97: 129, 129: 65}[expected]
            radio.press("ok")
            radio.wait_for(lambda v: v.get("spectrum.bins") == str(expected), sections=("channel",),
                           what="bandwidth point count")
        radio.press("down")
        _numeric(radio, 50)
        radio.wait_for(lambda v: v.get("spectrum.dwell_ms") == "50", sections=("channel",),
                       what="scan dwell setting")
        radio.press("down")
        radio.press("ok")
        receive_filter = "narrow" if value["spectrum.filter"] == "wide" else "wide"
        radio.wait_for(lambda v: v.get("spectrum.filter") == receive_filter, sections=("channel",),
                       what="receive filter toggle")
        radio.press("down")
        mode = {"off": "after_sweep", "after_sweep": "immediate", "immediate": "off"}[value["spectrum.auto_peak_mode"]]
        auto = "0" if mode == "off" else "1"
        radio.press("ok")
        changed = radio.wait_for(lambda v: v.get("spectrum.auto_peak_mode") == mode, sections=("channel",),
                                 what="automatic peak mode cycle")
        assert changed["spectrum.auto_peak"] == auto
        radio.press("back")
        scale = int(value["spectrum.scale_db"])
        scale += 20 if scale == -40 else -20
        radio.press("up" if int(value["spectrum.scale_db"]) == -40 else "down")
        shifted = radio.wait_for(lambda v: v.get("spectrum.scale_db") == str(scale), sections=("channel",),
                                 what="dBm window shift")
        assert int(shifted["spectrum.center_hz"]) == center
        radio.press("back")
        radio.press("0")
        reopened = radio.wait_for(lambda v: v.get("spectrum.active") == "1", sections=("channel",),
                                  what="remembered analyzer settings")
        assert reopened["spectrum.squelch"] == "0"
        assert reopened["spectrum.bins"] == value["spectrum.bins"]
        assert reopened["spectrum.step_hz"] == value["spectrum.step_hz"]
        assert reopened["spectrum.dwell_ms"] == "50"
        assert reopened["spectrum.auto_peak"] == auto
        assert reopened["spectrum.auto_peak_mode"] == mode
        assert reopened["spectrum.scale_db"] == str(scale)
        assert reopened["spectrum.filter"] == receive_filter
        radio.press("back")
        after = radio.wait_for(lambda v: v.get("spectrum.active") == "0", sections=("channel",),
                               what="receiver restored")
        assert after["ch.rx_hz"] == before["ch.rx_hz"]
        assert after["ch.mode"] == before["ch.mode"]
    finally:
        radio.press("back", LONG)


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
def test_spectrum_shortcut_assignments(radio):
    radio.go_home()
    before = radio.view(["channel"])
    if before.get("ch.mode") not in ("fm", "dmr") or before.get("ch.sfr") != "0,0":
        skip("state", "select a conventional RF channel without an SFR role")
    try:
        for key in ("ok", "6", "0", "ok"):
            radio.press(key)
        radio.wait_for(lambda v: v.get("ui.screen") == "spectrum-settings" and v.get("spectrum.active") == "1",
                       sections=("link", "channel"), what="spectrum settings")
        for _ in range(7):
            radio.press("down")
        for _ in range(4):
            if radio.view(["channel"])["spectrum.hash_key"] == "auto_peak":
                break
            radio.press("ok")
        radio.press("down")
        for _ in range(4):
            if radio.view(["channel"])["spectrum.star_key"] == "filter":
                break
            radio.press("ok")
        assigned = radio.view(["channel"])
        assert assigned["spectrum.hash_key"] == "auto_peak" and assigned["spectrum.star_key"] == "filter"
        radio.press("back")
        radio.press("context")
        next_mode = {"after_sweep": "immediate", "immediate": "off", "off": "after_sweep"}[assigned["spectrum.auto_peak_mode"]]
        radio.wait_for(lambda v: v.get("spectrum.auto_peak_mode") == next_mode and v.get("ui.screen") == "spectrum",
                       sections=("channel", "link"), what="# running its assigned action")
        radio.press("quick")
        next_filter = "wide" if assigned["spectrum.filter"] == "narrow" else "narrow"
        radio.wait_for(lambda v: v.get("spectrum.filter") == next_filter and v.get("ui.screen") == "spectrum",
                       sections=("channel", "link"), what="* running its assigned action")
        radio.press("back")
        radio.press("0")
        restored = radio.wait_for(lambda v: v.get("spectrum.active") == "1", sections=("channel",),
                                  what="remembered shortcut assignments")
        assert restored["spectrum.hash_key"] == "auto_peak" and restored["spectrum.star_key"] == "filter"
    finally:
        radio.press("back", LONG)


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
def test_spectrum_numeric_controls_without_keypad(radio):
    radio.go_home()
    try:
        for key in ("ok", "6", "0", "ok"):
            radio.press(key)
        before = radio.wait_for(lambda v: v.get("ui.screen") == "spectrum-settings",
                                sections=("link", "channel"), what="spectrum settings")
        if before.get("ui.count") != "10":
            skip("no_hw", "the board has an onboard keypad; numeric fallback is not used")
        original = int(before["spectrum.squelch"])
        key = "up" if original < 0 else "down"
        reverse = "down" if key == "up" else "up"
        expected = original + (1 if key == "up" else -1)
        for item in ("ok", key, "back"):
            radio.press(item)
        assert radio.view(["channel"])["spectrum.squelch"] == str(original)
        for item in ("ok", key, "ok"):
            radio.press(item)
        radio.wait_for(lambda v: v.get("spectrum.squelch") == str(expected), sections=("channel",),
                       what="arrow-edited squelch")
        for item in ("ok", reverse, "ok"):
            radio.press(item)
        radio.wait_for(lambda v: v.get("spectrum.squelch") == str(original), sections=("channel",),
                       what="original squelch restored")
        radio.press("up")
        last = radio.view(["link"])
        assert last["ui.focus"] == "9"  # Center is the last row only without a keypad.
    finally:
        radio.press("back", LONG)


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
def test_spectrum_refuses_active_diagnostic_detour(radio):
    from rfoundry_debug import DebugLinkError, detour_steps

    try:
        built = radio.view(["dmrip"]).get("dmrip.built") == "1"
    except DebugLinkError:
        built = False
    if not built:
        skip("not_built", "built without CONFIG_DEBUGLINK_DMRIP")
    radio.go_home()
    if radio.link.detour_status()["phase"] != "idle":
        skip("state", "another diagnostic detour is already running")
    try:
        radio.link.detour_run(15, detour_steps(["wait 10000"]))
        radio.wait_for(lambda _v: radio.link.detour_status().get("last", {}).get("armed"), sections=("dmrip",),
                       what="diagnostic detour armed")
        for key in ("ok", "6", "0"):
            radio.press(key)
        state = radio.wait_for(lambda v: v.get("ui.screen") == "spectrum", sections=("link", "channel"),
                               what="analyzer refusal screen")
        assert state["spectrum.active"] == "0"
    finally:
        radio.press("back", LONG)
        radio.link.detour_return()


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
@pytest.mark.parametrize("exit_key", ("back", "home"))
def test_spectrum_dual_watch_restore(radio, exit_key):
    if "DUAL_WATCH" not in radio.features():
        skip("not_built", "needs dual watch")
    radio.go_home()
    before = radio.core()
    size = int(before.get("zone_size", "0"))
    if before.get("vfo") == "1" or size < 2:
        skip("state", "select a conventional channel in a zone with at least two readable channels")
    other = (int(before["zone_pos"]) + 1) % size
    try:
        radio.link.scan("dual", other)
        radio.wait_for(lambda v: v.get("ui.dual") == "1", sections=("link",),
                       what="UI dual watch")
        radio.link.scan("select", 1)
        saved = radio.wait_for(lambda v: v.get("dual.sel") == "b", sections=("features",),
                               what="selected B pane")
        pair = {key: saved[key] for key in ("dual.a", "dual.b", "dual.sel")}
        radio.press("ok")
        radio.press("6")
        radio.press("0")
        paused = radio.wait_for(lambda v: v.get("spectrum.active") == "1" and
                               v.get("scan_phase") == "paused", sections=("channel", "features", "link"),
                               what="analyzer temporarily borrowing dual watch's receiver")
        assert paused["scan"] == "dual" and paused["ui.dual"] == "1"
        assert all(paused[key] == value for key, value in pair.items())
        radio.press("ok")
        radio.wait_for(lambda v: v.get("ui.screen") == "spectrum-settings", sections=("link",),
                       what="analyzer settings")
        if exit_key == "home":
            radio.press("back", LONG)
        else:
            radio.press("back")
            radio.press("back")
        resumed = radio.wait_for(lambda v: v.get("spectrum.active") == "0" and v.get("scan") == "dual",
                                sections=("channel", "features", "link"), what="dual watch resumed")
        assert resumed["ui.dual"] == "1"
        assert all(resumed[key] == value for key, value in pair.items())
    finally:
        radio.press("back", LONG)
        radio.link.scan("stop")


@pytest.mark.feature("spectrum")
@pytest.mark.needs_switch("SPECTRUM")
def test_spectrum_air_band_am_and_restore(radio):
    if not {"AM_RX", "VFO_FM"}.issubset(radio.features()):
        skip("not_built", "needs AM receive and the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    with radio.vfo_session():
        radio.link.vfo_set(118100000)
        radio.wait_for(lambda v: v.get("am") == "1" and v.get("rx_hz") == "118100000",
                       what="air-band VFO")
        try:
            radio.press("ok")
            radio.press("6")
            radio.press("0")
            radio.wait_for(lambda v: v.get("spectrum.active") == "1", sections=("channel",),
                           what="spectrum receiver ownership in the air band")
            for digits, hz, am in (("118100", 118100000, "1"), ("146520", 146520000, "0"),
                                   ("118100", 118100000, "1")):
                for digit in digits:
                    radio.press(digit)
                radio.press("ok")
                radio.wait_for(lambda v: v.get("spectrum.center_hz") == str(hz) and
                               v.get("spectrum.am") == am, sections=("channel",),
                               what="automatic analyzer AM/FM reception")
            radio.press("back")
            radio.wait_for(lambda v: v.get("spectrum.active") == "0", sections=("channel",),
                           what="analyzer exit")
            restored = radio.core()
            assert restored["am"] == "1" and restored["rx_hz"] == "118100000"
        finally:
            radio.press("back", LONG)


@pytest.mark.feature("spectrum", optional=True,
                     owner="feed an FM test carrier at the frequency printed by the test, then stop it")
@pytest.mark.needs_switch("SPECTRUM")
@pytest.mark.manual
@pytest.mark.rf
@pytest.mark.parametrize("mode", ("after_sweep", "immediate"))
@pytest.mark.parametrize("center_tuner", (False, True))
def test_spectrum_auto_capture_and_resume(radio, mode, center_tuner):
    radio.ensure_awake()
    radio.unlock()
    radio.press("back", LONG)
    before = radio.view(["channel"])
    if before.get("ch.mode") not in ("fm", "dmr") or before.get("ch.sfr") != "0,0":
        skip("state", "select a conventional RF channel without an SFR role")
    receiver = radio.core()
    if receiver.get("am") == "1" or receiver.get("bcast") == "1":
        skip("state", "the FM carrier test needs a channel outside the air and broadcast bands")
    try:
        radio.press("ok")
        radio.press("6")
        radio.press("0")
        active = radio.wait_for(lambda v: v.get("spectrum.active") == "1", sections=("channel",),
                                what="spectrum receiver ownership")
        # A previous test may have left the saved analyzer window in the air band.
        for digit in f"{int(before['ch.rx_hz']) // 10:08d}":
            radio.press(digit)
        radio.press("ok")
        radio.press("ok")
        _numeric(radio, 80)
        for _ in range(3):
            radio.press("down")
        _numeric(radio, 10)
        radio.press("down")
        radio.press("down")
        _auto_mode(radio, mode)
        radio.press("down")
        if (radio.view(["channel"])["spectrum.center_tuner"] == "1") != center_tuner:
            radio.press("ok")
        radio.press("back")
        active = radio.view(["channel"])
        center = int(active["spectrum.center_hz"])
        step = int(active["spectrum.step_hz"])
        target = center + 8 * step
        print(f"\n>>> Feed an FM test carrier at {target / 1e6:.5f} MHz, above squelch; keep it on", flush=True)
        caught = radio.wait_for(lambda v: v.get("spectrum.audio") == "1" and
                                abs(int(v.get("spectrum.tuner_hz", "0")) - target) <= 2 * step,
                                timeout=30, sections=("channel",), what="automatic carrier capture")
        assert int(caught["spectrum.center_hz"]) == center
        assert int(caught["spectrum.tuner_hz"]) != center
        time.sleep(0.5)
        held = radio.view(["channel"])
        assert held["spectrum.audio"] == "1"
        assert held["spectrum.tuner_hz"] == caught["spectrum.tuner_hz"]
        assert held["spectrum.index"] == caught["spectrum.index"]
        print("\n>>> Stop the FM test carrier", flush=True)
        resumed = radio.wait_for(lambda v: v.get("spectrum.audio") == "0" and
                                 v.get("spectrum.index") != held["spectrum.index"], timeout=30,
                                 sections=("channel",), what="sweep resuming after carrier loss")
        assert int(resumed["spectrum.center_hz"]) == center
        if center_tuner:
            assert int(resumed["spectrum.tuner_hz"]) == center
    finally:
        radio.press("back", LONG)
    after = radio.view(["channel"])
    assert after["ch.rx_hz"] == before["ch.rx_hz"]
    assert after["ch.mode"] == before["ch.mode"]
