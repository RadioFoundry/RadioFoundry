# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Host traffic on the debug link never makes a PTT press, radio only, never transmits.

On radios whose PTT pin is also the programming cable's host-to-radio serial line, every 0 bit the host sends pulls
the pin low. The firmware ignores the pin while bytes arrive and for 50 ms after, and then needs it low for 20 ms
(drivers/input/sharedptt, docs/spec/gotchas.md 16.2). The test polls the radio in bursts whose gaps fall on both sides
of the 50 ms gate, the pattern that keyed a radio during PPP (2026-10-22, hardware-dm32.md 6.2), with some requests back to back. The
engine's PTT press count (`input.ptt_downs`) must not move and the radio must never show `tx`. TX inhibit is on for
the run (RAM, put back after), so a press, if one came, transmits nothing. On a radio with no shared pin the test
passes trivially, so it runs on every board.
"""
import random
import time

import pytest

from odr_hil import skip

POLL_S = 30.0               # the owner's first gate check was 30 s of polling (hardware-db25d.md); the PPP
                            # failure made about one press a minute
GAP_MIN_S, GAP_MAX_S = 0.040, 0.120     # around the 50 ms gate
BACK_TO_BACK = 0.15         # share of requests sent with no gap


def ptt_downs(radio):
    v = radio.view(["link"])
    if "input.ptt_downs" not in v:
        skip("not_built", "this build has no input.ptt_downs")
    return int(v["input.ptt_downs"])


def test_host_traffic_no_ptt(radio):
    before_inhibit = radio.view().get("set.tx_inhibit")
    if before_inhibit is None:
        skip("not_built", "this build has no set.tx_inhibit")
    radio.link.setting("tx_inhibit", 1)
    try:
        radio.log()                     # empty the ring
        downs = ptt_downs(radio)
        rnd = random.Random(126)
        polls, states = 0, set()
        end = time.monotonic() + POLL_S
        while time.monotonic() < end:
            states.add(radio.core().get("state"))
            polls += 1
            if rnd.random() >= BACK_TO_BACK:
                time.sleep(rnd.uniform(GAP_MIN_S, GAP_MAX_S))
        after = ptt_downs(radio)
        _, log = radio.log()
    finally:
        time.sleep(0.06)                # the radio request rate limit
        radio.link.setting("tx_inhibit", int(before_inhibit))
    print(f"\n{polls} polls in {POLL_S:g} s, input.ptt_downs {downs} -> {after}, states {sorted(map(str, states))}")
    presses = [line for line in log.splitlines() if "input: PTT down" in line]
    assert after == downs, f"{after - downs} PTT press(es) during host traffic alone: {presses}"
    assert "tx" not in states, states
