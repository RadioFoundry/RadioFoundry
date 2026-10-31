# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Bluetooth PTT and keys (CONFIG_BT_PTT, hal_input_t.bt_stats, the debug link's input.bt_* keys;
docs/spec/radio-features.md 3.8).

test_bt_idle runs unattended: a radio with the module has it answer the start-up probe, no PTT is held at rest and few
bytes are dropped. A radio without one shows it absent and nothing held. test_bt_keys needs a person at the radio
(--manual) with a Bluetooth hand mic paired: it asks for each of the mic's keys and prints the code the module sends.
It never asks for PTT: that would transmit."""
import time

import pytest

from rfoundry_hil import as_int, skip

PRESSES = 4          # the module's key codes the stock firmware acts on: 01-04


def _bt(r):
    v = r.link.view(["link"])
    if "input.bt_present" not in v:
        skip("no_hw", "no Bluetooth module on this radio, or firmware without CONFIG_BT_PTT")
    return {k[len("input.bt_"):]: as_int(v[k]) for k in v if k.startswith("input.bt_")}


@pytest.mark.feature("bt_ptt")
def test_bt_idle(radio):
    radio.ensure_awake()
    a = _bt(radio)
    time.sleep(2.0)
    b = _bt(radio)
    print(f"    {a} -> {b}")
    assert b["ptt"] == 0, f"Bluetooth PTT held at rest (nobody touching a paired button?): {b}"
    if not b["present"]:
        return    # no module, or the APRS board answered: nothing more to see
    assert b["lines"] >= 1, b
    assert b["errors"] - a["errors"] <= 2, f"bytes dropped at rest: {a} -> {b}"


@pytest.mark.feature("bt_ptt", optional=True, owner="pair a Bluetooth hand mic with the original firmware, then press "
                     "the keys the test asks for")
@pytest.mark.manual
def test_bt_keys(radio):
    radio.ensure_awake()
    m = _bt(radio)
    if not m["present"]:
        skip("no_hw", "the Bluetooth module didn't answer")
    codes = []
    for i in range(PRESSES):
        before = m["keys"]
        print(f"\n>>> Press any key on the Bluetooth hand mic other than PTT (press {i + 1} of {PRESSES}, 15 s)",
              flush=True)
        end = time.monotonic() + 15.0
        while m["keys"] == before and time.monotonic() < end:
            time.sleep(0.1)
            m = _bt(radio)
        assert m["keys"] > before, f"no key event: {m}"
        codes.append(m["key"])
        print(f"    code {m['key']:02d}")
        assert m["ptt"] == 0, m
    assert all(1 <= c <= 4 for c in codes), codes
