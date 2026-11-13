# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR audio smoke: real audio both ways, on every radio with DMR, every bench pass (opt-in RF: --allow-rf, --ambe FILE /
$RFOUNDRY_HIL_AMBE; README.md "Default bench pass").

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

test_dmr_audio_rx_offset: the same call with the bench carrier OFFSET_HZ below and above the channel, one call each,
and the same pass criteria. A receiver whose DMR decoding falls off on one side of its channel fails here (the UV380
family's DMR receive offset, docs/spec/hardware-dm1701.md 5.13: 3 bits a frame at -500 Hz before it).

test_dmr_audio_rx_deviation: the same call from a transmitter DEV_LOW and DEV_HIGH of the standard's deviation, near
the receiver's sensitivity (the bench level set so the radio reads about RSSI_DBM; deviation is a level at the
demodulator's output, and near sensitivity a receiver has no level to spare). Pass: at least 95 % of the frames reach the
voice layer and the FEC corrects at most BITS_DEV a frame, both ways. The window between the two is narrow: a receive
audio gain 1 dB high corrects a bit or more a frame from a transmitter that over-deviates, 1-2 dB low loses sync to one
that under-deviates (docs/spec/dmr-voice.md, "Receive level"). Skips when the bench can't scale its deviation (info
feature dev_scale) or bring its level near RSSI_DBM at the radio.

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
import time

import pytest

from rfoundry_hil import counter_deltas, skip
from hil_config import rig

PRIME = 3                   # AMBE_VOICE_PRIME in core/dmr/ambe/ambeVoice.h
PEAK_MIN = 1000             # decoded speech peaks at thousands; the stale voice RAM gave 6
BIT_ERRORS_PER_FRAME = 2.0  # stale voice RAM: exactly 3 a frame; the DM-1801 Pro on the bench: about 1
TX_S = 3.0
OFFSET_HZ = 500             # bench carrier offset: a small share of the 12.5 kHz channel
DEV_LOW, DEV_HIGH = 0.85, 1.15  # transmitters 15 % under and over the standard's deviation
RSSI_DBM = -100             # the level the deviation test runs at: some 20 dB over the sensitivity of a DMR radio
BITS_DEV = 0.6              # FEC corrections a frame near sensitivity: about 0.2 inside the window, 1-1.5 past its edge
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
        skip("not_built", "DMR AUDIO NOT CHECKED: firmware built without DMR")
    freq = rig(pytestconfig, "dmr-freq")
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


def bench_call(radio, sdr_bench, freq, cc, dst, private, remote, offset_hz=0):
    """The bench sends the .ambe file remote (on the bench) as one call, its carrier offset_hz from freq; returns the
    radio's counter deltas and its dmr_stats view after the call."""
    radio.reset_counters()
    _, before = radio.counters()
    with sdr_bench.lease():
        sdr_bench.voice_call(freq, cc, dst, calls=1, ambe=remote, private=private, offset_hz=offset_hz)
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
    return 18 * -(-frames // 18)         # the bench_tx job pads a call to whole superframes with silence


@pytest.mark.rf
@pytest.mark.bench_service
def test_dmr_audio_rx(radio, sdr_bench, pytestconfig):
    freq, cc, dst, private, _ = dmr_channel(radio, pytestconfig)
    ambe = pytestconfig.getoption("--ambe")
    if not ambe:
        pytest.fail("the DMR audio smoke needs a speech recording: --ambe FILE or $RFOUNDRY_HIL_AMBE (9-byte frames)")
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


@pytest.mark.rf
@pytest.mark.bench_service
def test_dmr_audio_rx_offset(radio, sdr_bench, pytestconfig):
    freq, cc, dst, private, _ = dmr_channel(radio, pytestconfig)
    ambe = pytestconfig.getoption("--ambe")
    if not ambe or not os.path.isfile(ambe) or os.path.getsize(ambe) % 9:
        pytest.fail("the DMR audio smoke needs a speech recording: --ambe FILE or $RFOUNDRY_HIL_AMBE (9-byte frames)")
    frames = read_frames(ambe)
    sent = padded(len(frames))
    allowed = frame_repeats(frames) + (sent - len(frames)) + 2 + len(frames) // 50
    remote = sdr_bench.upload(ambe, "hil_audio_smoke.ambe")
    bad = []
    for off in (-OFFSET_HZ, OFFSET_HZ):
        d, stats = bench_call(radio, sdr_bench, freq, cc, dst, private, remote, offset_hz=off)
        good = max(1, d["voice.rx_in"] - d["voice.rx_bad"])
        print(f"carrier {off:+d} Hz: {d['voice.rx_bit_errors'] / good:.2f} bits a frame, radio {d}, "
              f"rx_peak {stats.get('voice.rx_peak')}")
        bad += check_played(d, stats, sent, allowed, f"carrier {off:+d} Hz")
    assert not bad, "; ".join(bad)


def quiet(radio, timeout=8.0):
    """Wait until the radio stops counting voice frames: a call that starts while it still follows the last one comes in
    at another slot timing"""
    t0, last, still = time.monotonic(), None, 0
    while time.monotonic() - t0 < timeout:
        n = radio.view(["dmr_stats"]).get("voice.rx_in")
        still = still + 1 if n == last else 0
        last = n
        if still >= 3:
            return
        time.sleep(0.2)


def rssi_in_call(radio, sdr_bench, freq, cc, dst, private, remote):
    """The radio's RSSI during a call of remote at the bench's current level (None: no call reached the voice layer)"""
    quiet(radio)
    before = int(radio.view(["dmr_stats"]).get("voice.rx_in", 0))
    with sdr_bench.lease():
        job = sdr_bench.voice_call(freq, cc, dst, calls=1, ambe=remote, private=private, background=True)
        rssi = None
        t0 = time.monotonic()
        while time.monotonic() - t0 < 6.0:
            time.sleep(0.3)
            if int(radio.view(["dmr_stats"]).get("voice.rx_in", 0)) > before + 10:
                rssi = int(radio.core().get("rssi_dbm", 0))
                break
        job.kill()
    quiet(radio)
    return rssi


@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.feature("dmr_voice")
def test_dmr_audio_rx_deviation(radio, sdr_bench, pytestconfig):
    freq, cc, dst, private, _ = dmr_channel(radio, pytestconfig)
    ambe = pytestconfig.getoption("--ambe")
    if not ambe or not os.path.isfile(ambe) or os.path.getsize(ambe) % 9:
        pytest.fail("the DMR audio smoke needs a speech recording: --ambe FILE or $RFOUNDRY_HIL_AMBE (9-byte frames)")
    if not sdr_bench.has("dev_scale"):
        skip("bench", "DMR RX DEVIATION NOT CHECKED: the bench board can't scale its deviation (info feature dev_scale)")
    frames = read_frames(ambe)
    sent = padded(len(frames))
    allowed = frame_repeats(frames) + (sent - len(frames)) + 2 + len(frames) // 50
    remote = sdr_bench.upload(ambe, "hil_audio_smoke.ambe")
    # The bench level: one call to read the radio's RSSI, then the level moved by the difference, checked once more
    level, saved = -40.0, (sdr_bench.power_dbm, sdr_bench.dev_scale)
    try:
        for _ in range(3):
            sdr_bench.power_dbm = level
            rssi = rssi_in_call(radio, sdr_bench, freq, cc, dst, private, remote)
            if rssi is None:
                level += 20                     # nothing heard: too weak to start a call
                continue
            print(f"bench {level:+.0f} dBm: radio RSSI {rssi} dBm")
            if abs(rssi - RSSI_DBM) <= 4:
                break
            level += RSSI_DBM - rssi
        else:
            skip("bench", f"DMR RX DEVIATION NOT CHECKED: the bench level doesn't bring the radio near {RSSI_DBM} dBm "
                          "(a board without power_dbm?)")
        bad = []
        for dev in (DEV_LOW, DEV_HIGH):
            sdr_bench.dev_scale = dev
            quiet(radio)
            d, stats = bench_call(radio, sdr_bench, freq, cc, dst, private, remote)
            good = max(1, d["voice.rx_in"] - d["voice.rx_bad"])
            per = d["voice.rx_bit_errors"] / good
            print(f"deviation x{dev:g}: {d['voice.rx_in']} of {sent} frames, {per:.2f} bits a frame, radio {d}")
            what = f"deviation x{dev:g} at {RSSI_DBM} dBm"
            if d["voice.rx_in"] < 0.95 * sent:
                bad.append(f"{what}: {d['voice.rx_in']} of {sent} frames reached the voice layer (at least 95 %)")
            if per > BITS_DEV:
                bad.append(f"{what}: {per:.2f} bits corrected a frame (at most {BITS_DEV:g})")
            bad += [b for b in check_played(d, stats, sent, allowed, what, bit_errors=False) if "frames into" not in b]
        assert not bad, "; ".join(bad)
    finally:
        sdr_bench.power_dbm, sdr_bench.dev_scale = saved


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
            # The bench board sends the file back into the local run directory
            lis = sdr_bench.listen_start(freq, cc, seconds=TX_S + 6,
                                         ambe_out=sdr_bench.run_path(rdir, "heard"))
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
    local = os.path.join(rdir, f"heard_{c['call']}.ambe")
    remote = sdr_bench.upload(local, os.path.basename(local))
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
