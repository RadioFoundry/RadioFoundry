# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A serial hand mic: a mic with its own MCU that sends its keys as frames (hal_input_t.hand_mic_stats, the debug
link's input.hmic_* keys; the radio's hardware page has the frame format).

test_hand_mic_idle runs unattended: with no mic the decoder must not see one, and with a mic in, frames keep coming,
no key or PTT is held and few bytes are dropped. test_hand_mic_keys needs a person at the radio (--manual): it asks
for the mic, then for each keypad key in turn, and prints the mic's code for each (the codes the hardware page lists
as inferred), then for the mic to be unplugged. It never asks for PTT: that would transmit."""
import time

import pytest

from rfoundry_hil import as_int, skip

KEYS = ("1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "*", "#")
GONE_S = 5.0        # the decoder's timeout with margin (3 s on the first radio)


def _mic(r):
    v = r.link.view(["link"])
    if "input.hmic_present" not in v:
        skip("no_hw", "no serial hand mic on this radio, or firmware without input.hmic_*")
    return {k[len("input.hmic_"):]: as_int(v[k]) for k in v if k.startswith("input.hmic_")}


def _wait(r, cond, timeout, prompt=None):
    if prompt:
        print(f"\n>>> {prompt} (waiting up to {timeout:.0f} s)", flush=True)
    end = time.monotonic() + timeout
    m = _mic(r)
    while not cond(m) and time.monotonic() < end:
        time.sleep(0.2)
        m = _mic(r)
    return m


@pytest.mark.feature("hand_mic")
def test_hand_mic_idle(radio):
    radio.ensure_awake()
    a = _mic(radio)
    time.sleep(3.0)
    b = _mic(radio)
    print(f"    {a} -> {b}")
    if not b["present"]:
        assert b["frames"] == a["frames"], f"frames accepted with no mic present: {a} -> {b}"
        assert b["ptt"] == 0 and b["key"] == 0, f"a key or PTT held with no mic: {b}"
        return
    assert b["frames"] > a["frames"], f"the mic is present but sent no frame in 3 s: {a} -> {b}"
    assert b["ptt"] == 0 and b["key"] == 0, f"a key or PTT held at rest (nobody touching the mic?): {b}"
    assert b["errors"] - a["errors"] <= 2, f"bytes dropped at rest: {a} -> {b}"


@pytest.mark.feature("hand_mic", optional=True, owner="plug in the keypad hand mic and press the keys the test asks for")
@pytest.mark.manual
def test_hand_mic_keys(radio):
    radio.ensure_awake()
    m = _wait(radio, lambda x: x["present"], 30.0, "Plug the keypad hand mic in (don't press PTT)")
    assert m["present"], f"no mic seen: {m}"
    codes = {}
    for k in KEYS:
        m = _wait(radio, lambda x: x["key"] != 0, 15.0, f"Press and hold {k} on the mic")
        assert m["key"] != 0, f"no key seen for {k}: {m}"
        raw = as_int(radio.link.view(["link"]).get("input.raw_keys"), 0)
        codes[k] = m["key"]
        print(f"    {k}: code 0x{m['key']:02X}, raw keys 0x{raw:08x}")
        assert raw != 0, f"key {k} (code 0x{m['key']:02X}) reached no key bit"
        m = _wait(radio, lambda x: x["key"] == 0, 15.0, "Let go")
        assert m["key"] == 0, f"key {k} still held after release: {m}"
    assert len(set(codes.values())) == len(KEYS), f"two keys share a code: {codes}"
    m = _wait(radio, lambda x: not x["present"], 30.0 + GONE_S, "Unplug the mic")
    assert not m["present"], f"the mic still counts as present after it was unplugged: {m}"
    assert m["ptt"] == 0 and m["key"] == 0, m
