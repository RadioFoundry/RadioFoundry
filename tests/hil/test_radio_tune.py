# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Radio tuning (radio-tune.md), no transmission.

test_saved_trims (--allow-reboot): trims saved in the settings ring (STORAGE TUNE_PUT, TUNE_SAVE) come back after a
restart, a settings save afterwards keeps them, and TUNE_CLEAR removes them for good. The table as found is saved
again at the end. Skipped on a radio without a settings ring or without radio tuning.

test_battery_trims: a gain trim scales the reading (`battery_mv` = raw + raw x gain / 1000, truncated, against the
core view's `tune` raw reading), a full voltage moved to the reading gives 100 %, and empty and full moved less than
400 mV apart are ignored (the charge follows the board's own). The stored record is put back (TUNE_LOAD) at the end.
Skipped on a radio without a battery or without radio tuning.
"""
import time

import pytest

from rfoundry_hil import DebugLinkError, as_int, skip
from rfoundry_debug import ST_UNAVAILABLE

W_BATT = 3
GAIN, EMPTY, FULL = 0, 1, 2
SPAN_MV = 400
CURVE = (0, 2, 4, 10, 22, 42, 60, 75, 88, 100)
# The battery is read each second outside transmissions: two readings and the link's round trips
READ_S = 4.0


def word(sel, value):
    return W_BATT << 14 | sel << 16 | (value & 0x1FFF) << 19


def pct(mv, empty, full, d_empty=0, d_full=0):
    """The core's charge curve (battery.c batt_pct)"""
    e, f = empty + d_empty, full + d_full
    if e > 0 and f >= e + SPAN_MV:
        empty, full = e, f
    if mv >= full:
        return 100
    if mv <= empty:
        return 0
    span, x = full - empty, (mv - empty) * (len(CURVE) - 1)
    i = x // span
    return CURVE[i] + (CURVE[i + 1] - CURVE[i]) * (x % span) // span


def reading(radio):
    """(raw mV, empty, full, corrected mV, percent), raw taken again after the features view so both come from one
    battery reading (a reading between the two sections is retried)"""
    for _ in range(5):
        t = radio.link.view(["core"]).get("tune")
        f = radio.link.view(["features"])
        t2 = radio.link.view(["core"]).get("tune")
        if t is None:
            return None
        if t == t2:
            raw, empty, full = (int(x) for x in t.split("/")[7:10])
            return raw, empty, full, int(f["battery_mv"]), int(f["battery_pct"])
        time.sleep(0.2)
    pytest.fail("the battery reading kept changing between view sections")


def settle(radio, words, check, what):
    radio.link.tune_set(words)
    end = time.monotonic() + READ_S
    while True:
        r = reading(radio)
        if check(*r):
            return r
        if time.monotonic() > end:
            pytest.fail(f"{what}: {r}")
        time.sleep(0.3)


@pytest.mark.feature("radio_tune")
def test_battery_trims(radio):
    first = radio.link.view(["core"]).get("tune")
    if first is None:
        skip("not_built", "the firmware has no radio tuning")
    if len(first.split("/")) < 10:
        skip("not_built", "the firmware has no battery trims (older firmware)")
    raw, empty, full, _, _ = reading(radio)
    if empty == 0:
        skip("no_hw", "the radio has no battery")
    assert raw > 0, "a radio with a battery reads one"
    try:
        settle(radio, [], lambda raw, e, f, mv, p: mv == raw and p == pct(mv, e, f), "no trims: the raw reading")
        settle(radio, [word(GAIN, 100)],
                   lambda raw, e, f, mv, p: mv == raw + raw * 100 // 1000 and p == pct(mv, e, f), "gain +100 permille")
        settle(radio, [word(GAIN, -50)],
                   lambda raw, e, f, mv, p: mv == raw - raw * 50 // 1000 and p == pct(mv, e, f), "gain -50 permille")
        # Full moved to the reading: 100 %, when a trim reaches and the span stays 400 mV or more
        raw, e, f, mv, _ = settle(radio, [], lambda raw, e, f, mv, p: mv == raw, "trims cleared")
        d_full = mv - f
        if -1000 <= d_full <= 1000 and mv - e >= SPAN_MV:
            settle(radio, [word(FULL, d_full)],
                   lambda raw, e, f, mv, p: p == pct(mv, e, f, 0, d_full) == 100,
                   f"full moved {d_full} mV")
        # Empty and full moved together by the most a trim allows: less than 400 mV apart (a board span under
        # 2400 mV), the span check keeps the board's voltages
        settle(radio, [word(EMPTY, 1000), word(FULL, -1000)],
               lambda raw, e, f, mv, p: p == pct(mv, e, f, 1000, -1000), "empty +1000, full -1000 mV")
    finally:
        radio.link.tune_load()
    time.sleep(0.06)


def name_entry(level, band, name):
    """A level name entry (radio-tune.md 3): kind 3, the level, the band index, 6 characters"""
    return bytes([3 << 4 | level, band]) + name.encode("ascii").ljust(6, b"\0")


def restart(radio):
    radio.wait_for(lambda s: s.get("settings.dirty") == "0", timeout=5.0, sections=("storage",),
                   what="settings saved")
    radio.version()     # known before the restart: reconnect() checks a radio on another port against it
    radio.link.reboot(0, radio.link.chip()["token"])
    radio.link.close()
    time.sleep(2.0)
    radio.reconnect(20.0)


@pytest.mark.feature("radio_tune")
@pytest.mark.reboot
def test_saved_trims(radio):
    if radio.link.view(["core"]).get("tune") is None:
        skip("not_built", "the firmware has no radio tuning")
    try:
        found = radio.link.tune_get()
        radio.link.tune_load()
    except DebugLinkError as e:
        if e.status == ST_UNAVAILABLE:
            skip("no_hw", "the radio has no settings ring of its own: trims work live only")
        raise
    # Names load on every board with a level 0 and a band 0, transmit or not
    want = [name_entry(0, 0, "HILT")]
    try:
        assert radio.link.tune_put(want) == 1
        assert radio.link.tune_save() == 1
        restart(radio)
        assert radio.link.tune_get() == want, "the saved trims after a restart"
        # A settings save afterwards (the same value again: nothing the owner set changes) keeps them
        tot = as_int(radio.view(["features"]).get("set.tot_s"), None)
        if tot is not None:
            radio.link.setting("tot_s", tot - 1 if tot else 1)
            radio.link.setting("tot_s", tot)
            restart(radio)
            assert radio.link.tune_get() == want, "the trims after a settings save and a restart"
        assert radio.link.tune_clear() == 0
        restart(radio)
        assert radio.link.tune_get() == [], "cleared trims after a restart"
    finally:
        if found:
            radio.link.tune_put(found)
            radio.link.tune_save()
        else:
            radio.link.tune_clear()
