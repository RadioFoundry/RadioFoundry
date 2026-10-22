# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The DMR VFO (docs/spec/dmr-voice.md 2.1), radio only, nothing transmitted: the VFO's mode, color code, slot and
talk target are set over the debug link (RADIO ops 31 and 15) and read back from the `core` and `dmr` views; they
are the VFO's own settings (no temporary flag), and a channel change and back keeps them. The VFO is put back to FM
with color code 1, slot 1 and talkgroup 9 (the defaults) afterwards, which is what a VFO that was never touched has."""
from odr_hil import skip


def test_vfo_dmr_settings(radio):
    if "VFO_DMR" not in radio.features() or "VFO_FM" not in radio.features():
        skip("not_built", "built without both VFO kinds")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    L = radio.link
    with radio.vfo_session():
        try:
            L.dmr_fields(mode="dmr", cc=7, ts=2)
            radio.wait_for(lambda v: v.get("mode") == "dmr" and v.get("dmr.cc") == "7" and v.get("dmr.ts") == "2",
                           sections=("core", "dmr"), what="DMR VFO on color code 7, slot 2")
            L.dmr_target(3100)
            v = radio.wait_for(lambda v: v.get("dmr.target_id") == "3100", sections=("core", "dmr"),
                               what="talk target 3100")
            assert v["temporary"] == "0" and v["dmr.target_manual"] == "0" and v["dmr.target_private"] == "0"
            assert v["vfo"] == "1" and v["dmr.active"] == "1"
            L.dmr_target(3107777, private=True)
            radio.wait_for(lambda v: v.get("dmr.target_id") == "3107777" and v.get("dmr.target_private") == "1",
                           sections=("dmr",), what="private target")
            # The VFO's own: a channel and back keeps all of it
            L.vfo(False)
            radio.wait_for(lambda v: v.get("vfo") == "0", what="a channel")
            L.vfo(True)
            v = radio.wait_for(lambda v: v.get("vfo") == "1" and v.get("mode") == "dmr", sections=("core", "dmr"),
                               what="the DMR VFO again")
            assert v["dmr.cc"] == "7" and v["dmr.ts"] == "2" and v["dmr.target_id"] == "3107777"
            assert v["dmr.target_private"] == "1" and v["temporary"] == "0"
            # Back to FM stops the call stack
            L.dmr_fields(mode="fm")
            radio.wait_for(lambda v: v.get("mode") == "fm" and v.get("dmr.active") == "0", sections=("core", "dmr"),
                           what="FM VFO")
        finally:
            L.dmr_fields(mode="dmr", cc=1, ts=1)
            radio.wait_for(lambda v: v.get("dmr.cc") == "1" and v.get("dmr.ts") == "1", sections=("dmr",),
                           what="color code 1, slot 1")
            L.dmr_target(9)
            radio.wait_for(lambda v: v.get("dmr.target_id") == "9", sections=("dmr",), what="talkgroup 9")
            L.dmr_fields(mode="fm")
            radio.wait_for(lambda v: v.get("mode") == "fm", what="FM VFO")
