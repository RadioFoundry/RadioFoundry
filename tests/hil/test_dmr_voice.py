# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR voice path end to end (opt-in RF: --allow-rf --allow-service-stop, and --ambe FILE / $ODR_HIL_AMBE): the SDR
bench sends one call carrying a real speech recording; every frame must reach the speaker path.

Pass criteria (README.md): the AMBE voice layer takes in every frame the bench sent (the file padded to whole
superframes), plays at least all but AMBE_VOICE_PRIME (3) of them (a frame can still be queued when the terminator
ends playback), and nothing is concealed, lost, dropped or overrun anywhere: call stack lost frames 0, voice
concealed / silence / lost / overflows 0, audio overruns and dropped blocks 0. The counters say where frames went
missing when it fails.

One exception: a single underrun after the call's last frame (voice.rx_underrun_at = the frames played). The queue
can run dry before the terminator reaches the voice layer, and that block repeats the last frame; nothing is missing.
It is a race with the terminator's processing time (the DM-32UV lost it in 2 of 3 runs)."""
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
    sent = 18 * -(-frames // 18)        # bench_tx.py pads to whole superframes
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
    at = radio.view(["dmr_stats"]).get("voice.rx_underrun_at")
    end_race = at is not None and d["voice.rx_underruns"] == 1 and int(at) == d["voice.rx_frames"]
    if end_race:
        print(f"one underrun after the last frame ({at}): the terminator came after the queue ran dry")
        assert d["voice.rx_concealed"] == 1, f"the end-of-call underrun concealed more than one block: {d}"
    for k in ("voice.rx_concealed", "voice.rx_silence", "voice.rx_lost", "voice.rx_overflows", "call.rx_lost_frames",
              "audio.ov", "audio.dr"):
        if k == "voice.rx_concealed" and end_race:
            continue
        assert d[k] == 0, f"{k} = {d[k]}: {d}"


def test_vocoder_encode_options(radio):
    """The vocoder's encode options (docs/spec/dmr-voice.md 6), radio only: the options in force follow the Mic noise
    reduction and Mic AGC settings (settings.audio_flags bit 0 = noise reduction off, bit 1 = AGC on; tone frames
    always on), and on a board that reports its codec word, the word is 0x800 plus 0x40 for noise reduction, 0x1000
    for tone frames and 0x2000 for AGC, never 0x100 (DC filter off) or 0x4000 (test mode)."""
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    v = radio.view(["dmr_stats", "storage"])
    if "voice.enc_supported" not in v:
        pytest.skip("firmware without the voice.enc_* view keys")
    supported = int(v["voice.enc_supported"], 0)
    if supported == 0:
        pytest.skip("this board's vocoder has no encode options")
    audio = int(v["settings.audio_flags"], 0)
    nr_on = (audio & 1) == 0
    want = (0x1 if nr_on else 0) | 0x2 | (0x4 if audio & 2 else 0)
    opts = int(v["voice.enc_options"], 0)
    print(f"enc_supported={supported:#x} enc_options={opts:#x} noise reduction {'on' if nr_on else 'off'} "
          f"enc_flags={v.get('voice.enc_flags')}")
    assert opts == want & supported, f"options in force {opts:#x}, expected {want & supported:#x}: {v}"
    if "voice.enc_flags" in v:
        flags = int(v["voice.enc_flags"], 0)
        expect = 0x800 | (0x40 if opts & 1 else 0) | (0x1000 if opts & 2 else 0) | (0x2000 if opts & 4 else 0)
        assert flags == expect, f"codec flags word {flags:#x}, expected {expect:#x}"
        assert flags & 0x4100 == 0, f"flags word {flags:#x} has a bit that must never be set"
