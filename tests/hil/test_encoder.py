# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The channel knob (docs/spec/hardware-dm1701.md 10.3, drivers/input/quadrature).

test_knob_direction runs unattended: detents injected over the debug link go through the same UI code as the knob's,
so it checks the directions (forward = next channel on Home, next row down in a list). test_knob_counts_detents needs
a person at the radio (--manual): it asks for turns and reads the decoder's own counts (input.enc_*), so it checks
the hardware side: one count per click, the steps per click the decoder assumes, no samples lost, none backward."""
import time

import pytest

from odr_hil import as_int, skip
from odr_debug import DOWN


def link(r):
    return r.link.view(["link"])


@pytest.fixture
def home(radio):
    radio.ensure_awake()
    if link(radio).get("ui.locked") == "1":
        skip("state", "the keys are locked")
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    radio.long_press("back")
    radio.wait_for(lambda v: v.get("ui.screen") == "home", sections=("link",), what="Home")
    yield radio
    radio.long_press("back")


@pytest.mark.feature("channel_knob")
def test_knob_direction(home):
    r = home
    v = r.core()
    if v.get("vfo") == "1" or as_int(v.get("zone_size"), 0) < 3:
        skip("state", "needs a zone of 3 or more channels, not the VFO")
    pos = as_int(v["zone_pos"])
    # Home: forward = the next channel
    r.press("enc", DOWN, 1)
    size = as_int(v["zone_size"])
    r.wait_for(lambda x: as_int(x["zone_pos"]) == (pos + 1) % size, what="next channel on a forward detent")
    r.press("enc", DOWN, -1)
    r.wait_for(lambda x: as_int(x["zone_pos"]) == pos, what="back on a backward detent")
    # A list (the Hub): forward = the next row down
    r.press("ok")
    s = r.wait_for(lambda x: x.get("ui.screen") == "hub", sections=("link",), what="the Hub")
    first = as_int(s["ui.focus"])
    r.press("enc", DOWN, 1)
    r.wait_for(lambda x: as_int(x["ui.focus"]) == first + 1, sections=("link",), what="focus down on forward")
    r.press("enc", DOWN, -1)
    r.wait_for(lambda x: as_int(x["ui.focus"]) == first, sections=("link",), what="focus up on backward")


def _counts(r):
    v = link(r)
    if "input.enc_detents" not in v:
        skip("not_built", "firmware without the encoder counters (input.enc_*)")
    return {k: as_int(v[k]) for k in ("input.enc_detents", "input.enc_transitions", "input.enc_illegal",
                                      "input.enc_reversals", "input.enc_steps", "input.encoder_total",
                                      "input.enc_deferred")}


def _turn(r, clicks, prompt, timeout=30.0):
    """Ask for clicks (signed), wait until the decoder has counted them or the person stops; returns the deltas."""
    before = _counts(r)
    print(f"\n>>> {prompt} (waiting up to {timeout:.0f} s)", flush=True)
    end = time.monotonic() + timeout
    last, quiet = before, 0.0
    while time.monotonic() < end:
        time.sleep(0.25)
        now = _counts(r)
        if clicks != 0 and now["input.enc_detents"] - before["input.enc_detents"] == clicks:
            time.sleep(1.0)     # a bounce or a click more after the last one would show here
            now = _counts(r)
            break
        quiet = quiet + 0.25 if now == last else 0.0
        if clicks != 0 and quiet >= 3.0 and now["input.enc_transitions"] != before["input.enc_transitions"]:
            break               # turned, then stopped
        last = now
    return {k: now[k] - before[k] for k in now}, now


@pytest.mark.feature("channel_knob", optional=True, owner="turn the channel knob by the clicks the test asks for")
@pytest.mark.manual
def test_knob_counts_detents(home):
    r = home
    d, now = _turn(r, 10, "Turn the channel knob 10 clicks CLOCKWISE, slowly")
    steps = now["input.enc_steps"]
    print(f"    {d}")
    assert d["input.enc_detents"] == 10, f"10 clicks counted as {d['input.enc_detents']}: {d}"
    assert d["input.enc_transitions"] == 10 * steps, \
        f"{d['input.enc_transitions']} steps for 10 clicks, the decoder assumes {steps} per click: {d}"
    assert d["input.encoder_total"] == 10 and d["input.enc_illegal"] <= 1, d
    d, now = _turn(r, -10, "Now 10 clicks COUNTERCLOCKWISE, fast")
    print(f"    {d}")
    assert d["input.enc_detents"] == -10, f"10 fast clicks back counted as {d['input.enc_detents']}: {d}"
    assert d["input.enc_illegal"] <= 2, f"samples lost on a fast turn: {d}"
    d, now = _turn(r, 0, "Now rock the knob back and forth around one click without letting it click over, 5 s",
                   timeout=6.0)
    print(f"    {d}")
    assert d["input.enc_detents"] == 0, f"a wobble counted {d['input.enc_detents']}: {d}"
