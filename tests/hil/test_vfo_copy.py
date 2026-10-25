# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Copy to VFO (docs/spec/dmr-voice.md 2.2), radio only, nothing transmitted: RADIO op 32 copies the channel on view to
the VFO and opens it. The channel's frequencies, mode, bandwidth and tones show in the VFO afterwards, the channel is
as it was, and the VFO's values stay across a channel and back. The VFO's frequency, bandwidth and tones are put back
afterwards, and its mode when it was DMR (an FM channel leaves the VFO's DMR color code, slot and target alone)."""
import pytest

from rfoundry_hil import DebugLinkError, skip


def _channel(radio):
    v = radio.view(["core", "channel"])
    return {k: v.get(k) for k in ("mode", "rx_hz", "tx_hz", "ch.bw", "ch.rx_tone", "ch.tx_tone", "channel_number")}


def _tone_args(text):
    kind, _, value = text.partition(":")
    return kind, int(value or 0)


@pytest.mark.feature("vfo")
def test_copy_to_vfo(radio):
    feats = radio.features()
    if "VFO_FM" not in feats:
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    L = radio.link
    with radio.vfo_session():
        before = _channel(radio)	# the VFO's own values, put back at the end
        L.vfo(False)
        radio.wait_for(lambda v: v.get("vfo") == "0", what="a channel")
        # The first FM channel of the zone
        pos = None
        for p in range(int(radio.core().get("zone_size") or 0)):
            L.zone_pos(p)
            v = radio.wait_for(lambda v: v.get("zone_pos") == str(p), what=f"position {p}")
            if v.get("mode") == "fm":
                pos = p
                break
        if pos is None:
            skip("state", "no FM channel in this zone")
        chan = _channel(radio)
        try:
            L.vfo_copy()
            v = radio.wait_for(lambda v: v.get("vfo") == "1", what="the VFO opened")
            assert v["temporary"] == "0" and v["channel_number"] == "0"
            now = _channel(radio)
            for k in ("mode", "rx_hz", "tx_hz", "ch.bw", "ch.rx_tone", "ch.tx_tone"):
                assert now[k] == chan[k], k
            # In the VFO already: nothing to do
            with pytest.raises(DebugLinkError):
                L.vfo_copy()
            # The VFO's values survive a channel and back; the channel is as it was
            L.vfo(False)
            radio.wait_for(lambda v: v.get("vfo") == "0" and v.get("zone_pos") == str(pos), what="the channel again")
            assert _channel(radio) == chan
            L.vfo(True)
            radio.wait_for(lambda v: v.get("vfo") == "1", what="the VFO again")
            now = _channel(radio)
            for k in ("mode", "rx_hz", "tx_hz", "ch.bw", "ch.rx_tone", "ch.tx_tone"):
                assert now[k] == chan[k], k
        finally:
            # Put the VFO's bandwidth and tones back (vfo_session restores its frequency)
            if radio.core().get("vfo") != "1":
                L.vfo(True)
                radio.wait_for(lambda v: v.get("vfo") == "1", what="the VFO")
            if "VFO_DMR" in feats and before["mode"] == "dmr":
                L.dmr_fields(mode="dmr")
            L.bandwidth(before["ch.bw"] == "25k")
            for which, key in ((2, "ch.rx_tone"), (1, "ch.tx_tone")):
                kind, value = _tone_args(before[key])
                L.tone(which, kind, value)
            radio.wait_for(lambda v: v.get("mode") == before["mode"] and _channel(radio)["ch.bw"] == before["ch.bw"],
                           what="the VFO's mode and bandwidth back")
