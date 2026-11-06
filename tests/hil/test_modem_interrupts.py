# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""No modem interrupt was lost since start (docs/spec/debuglink.md 3.7, the dmr section's dmr.modem_* keys).

Where the modem driver keeps them, it counts the transmissions it dropped because no slot interrupt came
(dmr.modem_stalls) and the requests it found pending with no handler run (dmr.modem_stuck). Either is a lost
interrupt: the driver recovers, but each one cost a call or a received burst. Read after the other tests (the file
name sorts after the DMR ones), so it covers what they did. Radio only, nothing transmitted; skipped on firmware or a
driver without the keys.
"""
import pytest

from rfoundry_hil import as_int, skip


@pytest.mark.feature("dmr_voice")
def test_no_lost_modem_interrupts(radio):
    kv = radio.view(["dmr"])
    if kv.get("dmr.built") == "0":
        skip("not_built", "firmware without DMR")
    if "dmr.modem_stalls" not in kv:
        skip("not_built", "a modem driver without the lost interrupt counts, or older firmware")
    stalls, stuck = as_int(kv["dmr.modem_stalls"]), as_int(kv["dmr.modem_stuck"])
    first = (kv.get("dmr.modem_lost_ms"), kv.get("dmr.modem_lost_regs"))
    print("lost modem interrupts: stalls %d, stuck %d, first %s" % (stalls, stuck, first))
    assert stalls == 0 and stuck == 0, f"lost modem interrupts: stalls {stalls}, stuck {stuck}, first at {first}"
