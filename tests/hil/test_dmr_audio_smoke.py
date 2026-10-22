# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR audio smoke: real audio both ways, on every radio with DMR, every bench pass (opt-in RF: --allow-rf, --ambe FILE /
$ODR_HIL_AMBE; README.md "Default bench pass").

Frame counters can't see audio. A modem that hands over the same stale voice bytes every burst (the HR_C7000
voice-access flag on the wrong slot: the DM-1801 Pro played every call silent, 2026-10-22) or a stuck encoder passes
every frame, lost and duplicate count. These tests look at the audio itself.

Skips and failures. Only a missing radio or SDR bench, or a run without --allow-rf, skips (with a reason that starts
"DMR AUDIO NOT CHECKED"), and so does a firmware built without DMR. Once the radio, its DMR and the bench are there,
anything else that keeps the check from running fails: no speech recording, no vocoder merged (dmr.codec=0), no
channel on --dmr-freq, firmware without the view keys or the test signal. The talkgroup comes from the radio: the
bench calls the channel's talk target (dmr.target_id, always listened to), or the radio's own ID privately when the
target is a private ID, on the channel's own color code, so a wrong --dmr-tg / --dmr-cc can't filter the call out.

test_dmr_audio_rx: the bench sends the speech recording as one call. Pass: the call is played (call.rx_calls_played
+1), at least 90 % of the frames sent reach the voice layer (voice.rx_in; the recording padded to whole superframes;
counting every frame is test_dmr_voice's job), the decoded audio peaks at PEAK_MIN or more (voice.rx_peak), the frames are the recording's and not one frame repeated
(voice.rx_repeats at most the recording's own repeats plus the padding plus 2 % of the frames), and the FEC corrected
at most 2 bits a frame on average (the stale RAM signature is exactly 3 a frame).

test_dmr_audio_tx (marker radio_tx): the radio keys TX_S seconds at its lowest power with the voice layer's test
signal in place of the mic (RADIO op 33: a tone whose pitch changes every frame), the bench modem records the call.
Pass: the bench heard a call from the radio's ID with at least 30 frames a second of PTT, at most 15 % of them the
AMBE silence frame and at most 10 % equal to the frame before, at least a tenth of them distinct (the test signal
repeats its cycle); the radio's encoder made as many frames with at most 10 % repeats. Then the bench plays the recorded call back to the radio as in
test_dmr_audio_rx: the radio's own decoder must find audio in what it sent (rx_peak at least PEAK_MIN). The mic level
the board delivered during the over (voice.tx_mic_peak) is printed: with a speaker-mic cable that cuts the mic it is
0, and that alone doesn't fail. The test signal is turned off in a finally block (it is RAM only).
"""
import os
import subprocess
import time

import pytest

from odr_hil import counter_deltas

PRIME = 3                   # AMBE_VOICE_PRIME in core/dmr/ambe/ambeVoice.h
PEAK_MIN = 1000             # decoded speech peaks at thousands; the stale voice RAM gave 6
BIT_ERRORS_PER_FRAME = 2.0  # stale voice RAM: exactly 3 a frame; the DM-1801 Pro on the bench: about 1
TX_S = 3.0
ALL_CALL = 0xFFFFFF
STATS_KEYS = ("voice.rx_peak", "voice.rx_repeats", "voice.tx_repeats", "voice.tx_mic_peak", "voice.tx_test")

pytestmark = [pytest.mark.audio_smoke]


def frame_repeats(frames):
    """Frames equal to the one before."""
    return sum(1 for a, b in zip(frames, frames[1:]) if a == b)


def read_frames(path):
    data = open(path, "rb").read()
    return [data[i:i + 9] for i in range(0, len(data) - len(data) % 9, 9)]


def find_dmr_channel(radio, rx_hz):
    """Select a DMR channel on rx_hz: in the current zone first, then in the others (the radio fixture puts the zone
    back). Fails when no zone has one."""
    try:
        return radio.find_channel(rx_hz, "dmr")
    except AssertionError:
        pass
    core = radio.core()
    here, count = int(core.get("zone_index") or 0), int(core.get("zone_count") or 0)
    for z in [i for i in range(count) if i != here]:
        radio.link.zone(z)
        radio.wait_for(lambda v: v.get("zone_index") == str(z), what=f"zone {z}")
        try:
            pos = radio.find_channel(rx_hz, "dmr")
            print(f"DMR channel on {rx_hz / 1e6:.5f} MHz in zone {z} ({radio.core().get('zone_name')})")
            return pos
        except AssertionError:
            continue
    pytest.fail(f"no DMR channel on {rx_hz / 1e6:.5f} MHz in any of the {count} zones: give --dmr-freq a frequency "
                "the codeplug has a DMR channel on")


def dmr_channel(radio, pytestconfig):
    """Put the radio on its DMR channel at --dmr-freq and work out the call that reaches its speaker. Returns
    (freq, cc, dst, private, own_id). Fails (not skips) when the radio has DMR but can't play a call."""
    if "DMR" not in radio.features():
        pytest.skip("DMR AUDIO NOT CHECKED: firmware built without DMR")
    freq = pytestconfig.getoption("--dmr-freq")
    radio.go_home()
    find_dmr_channel(radio, round(freq * 1e6))
    d = radio.view(["dmr"])
    if d.get("dmr.codec") != "1":
        pytest.fail("no vocoder merged in this firmware (dmr.codec=0): DMR voice plays and sends nothing. Merge the "
                    "radio's codec (docs/user) before the bench pass")
    stats = radio.view(["dmr", "dmr_stats"])
    missing = [k for k in STATS_KEYS if k not in stats]
    if missing:
        pytest.fail(f"firmware without the view keys {missing}: the audio checks can't run (an older build?)")
    own, target = int(d.get("dmr.own_id", 0)), int(d.get("dmr.target_id", 0))
    private = d.get("dmr.target_private") == "1"
    cc = int(radio.view(["channel"])["ch.color_code"])
    if target and not private:
        dst, private = target, False            # the channel's talk target is always listened to
    elif own:
        dst, private = own, True                # a private call to us
    else:
        dst, private = ALL_CALL, False
    print(f"channel at {freq} MHz: cc {cc}, own ID {own}, talk target {target}{' (private)' if d.get('dmr.target_private') == '1' else ''}"
          f" -> the bench calls {'private ' if private else 'TG '}{dst}")
    return freq, cc, dst, private, own


def bench_call(radio, sdr_bench, freq, cc, dst, private, remote):
    """The bench sends the .ambe file remote (on the bench) as one call; returns the radio's counter deltas and its
    dmr_stats view after the call."""
    radio.reset_counters()
    _, before = radio.counters()
    with sdr_bench.lease():
        sdr_bench.voice_call(freq, cc, dst, calls=1, ambe=remote, private=private)
    time.sleep(2.0)                      # playback drains (the queue holds 160 ms)
    _, after = radio.counters()
    keys = ["voice.rx_in", "voice.rx_frames", "voice.rx_repeats", "voice.rx_bit_errors", "voice.rx_bad",
            "voice.rx_lost", "call.rx_calls", "call.rx_calls_played", "call.rx_voice_bursts", "modem.voice"]
    return counter_deltas(before, after, keys), radio.view(["dmr_stats"])


def check_played(d, stats, sent, allowed_repeats, what, bit_errors=True):
    """The audio checks on one played call: a list of failure strings. bit_errors: judge the FEC corrections too (not
    for a recording the bench modem made off air, which carries the bit errors it heard)."""
    bad = []
    if d["call.rx_calls_played"] < 1:
        bad.append(f"{what}: the call was not played (call.rx_calls {d['call.rx_calls']}, played "
                   f"{d['call.rx_calls_played']}): filtered out?")
    if d["voice.rx_in"] < 0.9 * sent:      # every frame counted is test_dmr_voice's job; here enough to judge audio
        bad.append(f"{what}: frames into the voice layer {d['voice.rx_in']} of {sent} sent (at least 90 %)")
    peak = int(stats.get("voice.rx_peak", 0))
    if peak < PEAK_MIN:
        bad.append(f"{what}: decoded audio peaks at {peak} < {PEAK_MIN}: silent output (stale or wrong voice bits)")
    if d["voice.rx_repeats"] > allowed_repeats:
        bad.append(f"{what}: {d['voice.rx_repeats']} of {d['voice.rx_in']} frames repeat the one before (at most "
                   f"{allowed_repeats}): the same voice bytes again and again")
    good = max(1, d["voice.rx_in"] - d["voice.rx_bad"])
    if bit_errors and d["voice.rx_bit_errors"] > BIT_ERRORS_PER_FRAME * good:
        bad.append(f"{what}: {d['voice.rx_bit_errors']} bits corrected in {good} frames (at most "
                   f"{BIT_ERRORS_PER_FRAME:g} a frame)")
    return bad


def padded(frames):
    return 18 * -(-frames // 18)         # bench_tx.py pads a call to whole superframes with silence


@pytest.mark.rf
@pytest.mark.bench_service
def test_dmr_audio_rx(radio, sdr_bench, pytestconfig):
    freq, cc, dst, private, _ = dmr_channel(radio, pytestconfig)
    ambe = pytestconfig.getoption("--ambe")
    if not ambe:
        pytest.fail("the DMR audio smoke needs a speech recording: --ambe FILE or $ODR_HIL_AMBE (9-byte frames)")
    if not os.path.isfile(ambe) or os.path.getsize(ambe) % 9:
        pytest.fail(f"{ambe}: not a 9-byte-frame .ambe file")
    frames = read_frames(ambe)
    sent = padded(len(frames))
    allowed = frame_repeats(frames) + (sent - len(frames)) + 2 + len(frames) // 50
    remote = sdr_bench.upload(ambe, "hil_audio_smoke.ambe")
    d, stats = bench_call(radio, sdr_bench, freq, cc, dst, private, remote)
    print(f"sent {sent} frames ({len(frames)} in the file, {frame_repeats(frames)} repeats there); radio: {d}, "
          f"rx_peak {stats.get('voice.rx_peak')}")
    bad = check_played(d, stats, sent, allowed, "bench speech")
    assert not bad, "; ".join(bad)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_audio_tx(radio, sdr_bench, pytestconfig, tmp_path):
    freq, cc, dst, private, own = dmr_channel(radio, pytestconfig)
    if own == 0:
        pytest.fail("the radio has no DMR ID (dmr.own_id=0): it can't transmit DMR")
    radio.lowest_power()
    rdir = sdr_bench.run_dir("hil_audio_smoke_tx")
    radio.reset_counters()
    _, before = radio.counters()
    try:
        radio.link.voice_test(True)
        assert radio.view(["dmr"]).get("voice.tx_test") == "1", "the voice test signal didn't turn on"
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=TX_S + 6, ambe_out=f"~/{sdr_bench.dir}/{rdir}/heard")
            radio.ptt_for(TX_S)
            radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
            heard = sdr_bench.listen_result(lis)
    finally:
        radio.link.voice_test(False)
    _, after = radio.counters()
    tx = counter_deltas(before, after, ["voice.tx_frames", "voice.tx_repeats", "call.tx_calls"])
    mic_peak = radio.view(["dmr"]).get("voice.tx_mic_peak")
    calls = sorted((r for r in heard if "call" in r and r.get("src") == own), key=lambda r: -r.get("ambe_frames", 0))
    print(f"radio: {tx}, mic peak during the over {mic_peak} (0: no mic, e.g. a speaker-mic cable that cuts it)")
    print(f"bench heard: {[{k: r.get(k) for k in ('call', 'src', 'dst', 'voice', 'ambe_frames', 'ambe_silence_frames', 'ended')} for r in heard if 'call' in r]}")
    min_frames = int(30 * TX_S)
    assert tx["call.tx_calls"] == 1, f"the radio didn't make one TX call: {tx}"
    assert tx["voice.tx_frames"] >= min_frames, f"the encoder made {tx['voice.tx_frames']} frames in {TX_S} s"
    assert tx["voice.tx_repeats"] <= tx["voice.tx_frames"] // 10, \
        f"the encoder repeated {tx['voice.tx_repeats']} of {tx['voice.tx_frames']} frames with the test signal: stuck"
    assert calls, f"the bench modem heard no call from the radio's ID {own}"
    c = calls[0]
    assert any(f.endswith(".ambe") for f in c.get("files", [])), f"the bench recorded no AMBE for the call: {c}"
    remote = f"{rdir}/heard_{c['call']}.ambe"           # relative to the bench directory, as upload() gives
    local = tmp_path / "heard.ambe"
    subprocess.run(["scp", "-q", f"{sdr_bench.host}:{sdr_bench.dir}/{remote}", str(local)], check=True, timeout=120)
    frames = read_frames(local)
    n = len(frames)
    rep, distinct, silence = frame_repeats(frames), len(set(frames)), c.get("ambe_silence_frames", 0)
    print(f"on air: {n} frames, {distinct} distinct, {rep} repeat the one before, {silence} AMBE silence")
    bad = []
    if n < min_frames:
        bad.append(f"the bench heard {n} frames of a {TX_S} s over (at least {min_frames})")
    if silence > 0.15 * n:
        bad.append(f"{silence} of {n} frames on air are AMBE silence: the test signal didn't reach the air")
    if rep > n // 10:
        bad.append(f"{rep} of {n} frames on air repeat the one before: the same voice bytes sent again and again")
    if distinct < n // 10:                # the test signal repeats its cycle, so frames recur, but not few of them
        bad.append(f"only {distinct} distinct frames of {n} on air")
    assert not bad, "; ".join(bad)

    # The radio's own decoder on what it sent: the bench plays the recording back
    sent = padded(n)
    d, stats = bench_call(radio, sdr_bench, freq, cc, dst, private, remote)
    print(f"played back {sent} frames: radio {d}, rx_peak {stats.get('voice.rx_peak')}")
    bad = check_played(d, stats, sent, rep + (sent - n) + 2 + n // 50, "the radio's own over, played back",
                        bit_errors=False)
    assert not bad, "; ".join(bad)
