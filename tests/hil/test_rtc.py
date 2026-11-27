# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The board's hardware clock (docs/spec/radio-features.md 5.6, debuglink.md 3.31). Nothing transmits.

The test sets the clock to this computer's UTC time, reads it back after the radio's settle time and again after a few
seconds, and checks that it counts in step with this computer. It leaves the clock set to the right time. Skips on a
firmware without CONFIG_RTC and on a radio without a clock driver.

Run from a bench only: it writes the radio's clock (nothing in the codeplug or the settings).
"""
import time

import pytest

from rfoundry_debug import DebugLinkError
import rfoundry_hil

SETTLE_S = 2.0		# the clock takes a loaded time within a second (docs/spec/hardware-dm32.md 3.3)
TOLERANCE_S = 3		# this computer's time against the radio's, with the settle time and the bus


def clock(radio):
    try:
        return radio.link.rtc_get()
    except DebugLinkError as e:
        if e.status in (1, 4):
            rfoundry_hil.skip("not_built", "firmware without a hardware clock (CONFIG_RTC off, no driver, or older)")
        raise


@pytest.mark.feature("rtc")
def test_rtc_set_and_counts(radio):
    clock(radio)
    now = int(time.time())
    radio.link.rtc_set(now)
    time.sleep(SETTLE_S)
    valid, t1 = radio.link.rtc_get()
    assert valid, "the clock holds no time after it was set"
    assert abs(t1 - (time.time())) <= TOLERANCE_S + SETTLE_S, f"set {now}, read back {t1}"
    time.sleep(4)
    valid, t2 = radio.link.rtc_get()
    assert valid and 3 <= t2 - t1 <= 6, f"the clock counted {t2 - t1} s in about 4 s"
