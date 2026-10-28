# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The reference trim (radio-features.md 6.1) over the debug link's SETTING op 35, RAM only: no RF.

A trim in range is taken, rounded to 100 Hz, and shows as `ref_trim` in the storage section; past +-3000 Hz it is
refused (bad argument) and the trim in effect stays. The stored setting (`settings.ref_trim`) isn't changed, and the
trim in effect before is put back. Skipped on a radio without the trim (`ref_trim=-`). Whether the carrier moves is
checked off air (the hardware page's bench notes), not here.
"""
import time

import pytest

from rfoundry_hil import skip
from rfoundry_debug import DebugLinkError


def trim_view(radio):
    v = radio.link.view(["storage"])
    return v.get("ref_trim"), v.get("settings.ref_trim")


@pytest.mark.feature("ref_trim")
def test_ref_trim_setting(radio):
    now, stored = trim_view(radio)
    if now is None:
        skip("not_built", "the firmware has no reference trim keys (older firmware)")
    if now == "-":
        skip("no_hw", "the radio's board has no reference trim (hal_radio_t.ref_trim)")
    before = int(now)
    try:
        radio.link.setting("ref_trim", -1240)
        time.sleep(0.06)        # the radio request rate limit
        assert trim_view(radio) == ("-1200", stored), "a trim in range is taken, rounded to 100 Hz, not stored"
        radio.link.setting("ref_trim", 3000)
        time.sleep(0.06)
        assert trim_view(radio)[0] == "3000"
        with pytest.raises(DebugLinkError):
            radio.link.setting("ref_trim", 3100)
        time.sleep(0.06)
        with pytest.raises(DebugLinkError):
            radio.link.setting("ref_trim", -3100)
        time.sleep(0.06)
        assert trim_view(radio) == ("3000", stored), "a refused trim leaves the one in effect"
    finally:
        radio.link.setting("ref_trim", before)
        time.sleep(0.06)
    assert trim_view(radio) == (str(before), stored)
