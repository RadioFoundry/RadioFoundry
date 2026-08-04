# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR voice path end to end (opt-in RF: --allow-rf --allow-service-stop, and --ambe FILE / $ODR_HIL_AMBE): the SDR
bench sends one call carrying a real speech recording; every frame must reach the speaker path.

Pass criteria (README.md): the AMBE voice layer takes in every frame the bench sent (the file padded to whole
superframes), plays at least all but AMBE_VOICE_PRIME (3) of them (a frame can still be queued when the terminator
ends playback), and nothing is concealed, lost, dropped or overrun anywhere: call stack lost frames 0, voice
concealed / silence / lost / overflows 0, audio overruns and dropped blocks 0. The counters say where frames went
missing when it fails."""
import os
import time

import pytest

from odr_hil import counter_deltas

PRIME = 3   # AMBE_VOICE_PRIME in core/dmr/ambe/ambeVoice.h


@pytest.mark.rf
@pytest.mark.bench_service
def test_dmr_voice_speech(radio, sdr_bench, pytestconfig):
    ambe = pytestconfig.getoption("--ambe")
    if not ambe:
        pytest.skip("give --ambe FILE (or $ODR_HIL_AMBE): a 9-byte-frame .ambe speech recording")
    if not os.path.isfile(ambe) or os.path.getsize(ambe) % 9:
        pytest.fail(f"{ambe}: not a 9-byte-frame .ambe file")
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    frames = os.path.getsize(ambe) // 9
    sent = 18 * -(-frames // 18)        # hat_tx.py pads to whole superframes
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    if radio.view(["dmr"]).get("dmr.codec") != "1":
        pytest.skip("no vocoder merged in this firmware (dmr.codec=0): voice can't play")
    remote = sdr_bench.upload(ambe, "hil_speech.ambe")
    time.sleep(1.0)
    radio.reset_counters()
    _, before = radio.counters()
    with sdr_bench.lease():
        sdr_bench.voice_call(freq, cc, tg, calls=1, ambe=remote)
    time.sleep(2.0)                      # playback drains (the queue holds 160 ms)
    _, after = radio.counters()
    keys = ["voice.rx_in", "voice.rx_frames", "voice.rx_concealed", "voice.rx_silence", "voice.rx_lost",
            "voice.rx_overflows", "voice.rx_bad", "voice.rx_underruns", "call.rx_lost_frames", "call.rx_voice_bursts",
            "call.rx_late_entries", "audio.ov", "audio.dr", "modem.voice", "modem.crc"]
    d = counter_deltas(before, after, keys)
    print(f"sent {sent} frames ({frames} in the file); radio deltas: {d}")
    assert d["call.rx_late_entries"] == 0, f"the call started late (header missed): {d}"
    assert d["voice.rx_in"] == sent, f"frames into the voice layer {d['voice.rx_in']} != {sent}: {d}"
    assert sent - PRIME <= d["voice.rx_frames"] <= sent, f"frames played {d['voice.rx_frames']} of {sent}: {d}"
    for k in ("voice.rx_concealed", "voice.rx_silence", "voice.rx_lost", "voice.rx_overflows", "call.rx_lost_frames",
              "audio.ov", "audio.dr"):
        assert d[k] == 0, f"{k} = {d[k]}: {d}"
