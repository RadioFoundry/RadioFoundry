# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""USRP voice over UDP in USB network mode (opt-in: --allow-usb-switch; firmware with CONFIG_NET_USRP,
docs/spec/usb-audio.md 4.2). Each test switches the radio to the network adapter, talks USRP to it with
tools/usrp_client.py from the host's side of the link, and switches it back to serial.

  test_usrp_fm_rx    the FM VFO on NOAA (--noaa-mhz) with the squelch open: keyup frames with audio reach the host,
                     and a keyup 0 frame follows when the squelch closes (a quiet frequency). Nothing transmitted.
                     Optional: it needs NOAA strong enough at the bench
  test_usrp_fm_rx_bench  the same with the bench board's FM transmitter (--allow-rf) on --fm-mhz, a 1 kHz tone: keyup
                     frames with audio while it sends, a keyup 0 frame when its carrier ends and the squelch closes
  test_usrp_dmr_rx   with the SDR bench (--allow-rf: the bench transmits): a bench voice call on the --dmr-freq
                     channel reaches the host as keyup frames of decoded audio for the call's length
  test_usrp_fm_tx    radio_tx: the FM VFO on --fm-mhz, lowest power; a 1 kHz tone from the host keys the radio and
                     goes on air (the SDR bench measures it); the radio unkeys on keyup 0, and on its own when the
                     host stops sending (the keyup watchdog)"""
import contextlib
import os
import sys
import time

import pytest

import fm_measure
from rfoundry_hil import skip

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import usrp_client  # noqa: E402

from test_net_services import network_mode  # noqa: E402
from hil_config import rig


@contextlib.contextmanager
def usrp_session(radio):
    """The radio on the network: yields (debug link over UDP, a USRP peer); radio.link is the UDP link meanwhile so
    the Radio helpers work"""
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    serial = radio.link
    with network_mode(radio) as (udp, radio_ip, _):
        if "usrp.peer" not in udp.view(["usrp"]):
            skip("not_built", "built without CONFIG_NET_USRP")
        peer = usrp_client.Peer(radio_ip)
        radio.link = udp
        try:
            yield udp, peer
        finally:
            peer.close()
            radio.link = serial
    # Back on serial: the receive tap is off and the audio stream stopped with it (nothing fills the ring)
    v = usrp_view(radio.link)
    time.sleep(1.0)
    w = usrp_view(radio.link)
    assert v.get("usrp.rx_on") == "0", f"the receive tap stayed on after the network session: {v}"
    assert w["mux.rx_samples"] == v["mux.rx_samples"], f"audio still flows to the host ring: {v} -> {w}"


def usrp_view(link):
    return link.view(["usrp"])


def keyed(rows):
    return [r for r in rows if r[2] and r[3]]


def check_fm_frames(rows, v):
    """5 s of frames with the squelch open: keyup frames, audio in them, few lost"""
    k = keyed(rows)
    print(f"\n{len(rows)} frames in 5 s, {len(k)} with keyup; " + " ".join(f"{n}={v[n]}" for n in sorted(v)))
    assert len(k) >= 200, f"expected about 250 keyup frames in 5 s, got {len(k)}"
    levels = sorted(usrp_client.rms(r[3]) for r in k)
    print(f"audio rms: median {levels[len(levels) // 2]:.0f}, max {levels[-1]:.0f}")
    assert levels[len(levels) // 2] > 50, "keyup frames but no audio in them"
    seqs = [r[1] for r in rows]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    assert gaps <= 3, f"{gaps} sequence gaps (frames lost on the link)"
    assert int(v["usrp.peer"].split(".")[0]) != 0, v


def check_closed(rows):
    """The frames once the squelch closed: one keyup 0 frame, then nothing"""
    assert len(keyed(rows)) <= 25 and (not rows or not rows[-1][2]), \
        f"{len(rows)} frames after the squelch closed, the last with keyup {rows[-1][2] if rows else '-'}"


@pytest.mark.feature("net_usrp", optional=True)
@pytest.mark.usb_switch
def test_usrp_fm_rx(radio, pytestconfig):
    if not pytestconfig.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    hz = round(rig(pytestconfig, "noaa-mhz") * 1e6)
    radio.skip_out_of_band(hz)
    with usrp_session(radio) as (link, peer):
        with radio.vfo_session():
            link.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
            try:
                radio.wait_for(lambda v: v.get("state") == "rx", 10, interval=0.2, what="NOAA opens the squelch")
            except AssertionError:
                skip("bench", f"NOAA on {hz / 1e6:.3f} MHz doesn't open the squelch here")
            peer.hello()
            rows = peer.receive(5)
            check_fm_frames(rows, usrp_view(link))
            # A quiet frequency: the squelch closes, one keyup 0 frame, then nothing
            link.vfo_set(hz + 25000)
            radio.wait_for(lambda x: x.get("state") == "idle", 5, what="squelch closed")
            check_closed(peer.receive(2))


BENCH_FM_S = 8.0        # the bench's 1 kHz tone: the 5 s window after the squelch opens, under the 10 s key limit


@pytest.mark.feature("net_usrp")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
def test_usrp_fm_rx_bench(radio, sdr_bench, pytestconfig):
    if not pytestconfig.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    program = [{"kind": "tone", "hz": 1000, "s": BENCH_FM_S, "level": 0.6}]
    with usrp_session(radio) as (link, peer):
        with radio.vfo_session():
            link.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
            peer.hello()
            with sdr_bench.lease():
                tx = sdr_bench.fm_tx_start(mhz, program)
                radio.wait_for(lambda v: v.get("state") == "rx", 3, interval=0.1,
                               what="the bench's carrier opens the squelch")
                rows = peer.receive(5)
                check_fm_frames(rows, usrp_view(link))
                while tx.poll() is None:          # the rest of the tone, frames read so they don't queue up
                    peer.receive(0.2)
                done = tx.wait(5)
                print(f"bench {done}")
                assert not done.get("cut"), f"the bench cut its transmission: {done}"
            # The carrier gone: the squelch closes, one keyup 0 frame, then nothing
            rows = peer.receive(0.5)
            radio.wait_for(lambda x: x.get("state") == "idle", 5, what="squelch closed")
            check_closed(rows + peer.receive(2))


@pytest.mark.feature("net_usrp")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_usrp_dmr_rx(radio, sdr_bench, pytestconfig):
    if not pytestconfig.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    mhz = rig(pytestconfig, "dmr-freq")
    tg = rig(pytestconfig, "dmr-tg")
    ambe = pytestconfig.getoption("--ambe")
    with usrp_session(radio) as (link, peer):
        radio.find_channel(round(mhz * 1e6), "dmr")
        cc = rig(pytestconfig, "dmr-cc")
        remote = sdr_bench.upload(ambe, "hil_speech.ambe") if ambe else None
        peer.hello()
        with sdr_bench.lease():
            import threading
            got = []
            t = threading.Thread(target=lambda: got.extend(peer.receive(14)))
            t.start()
            time.sleep(0.5)
            sdr_bench.voice_call(mhz, cc, tg, superframes=10, ambe=remote)
            t.join()
        v = usrp_view(link)
        k = keyed(got)
        print(f"\n{len(got)} frames, {len(k)} with keyup; " + " ".join(f"{x}={v[x]}" for x in sorted(v)))
        # 10 superframes = 3.6 s of voice: about 180 frames of 20 ms
        assert len(k) >= 120, f"expected about 180 keyup frames for the call, got {len(k)}"
        levels = sorted(usrp_client.rms(r[3]) for r in k)
        assert levels[len(levels) // 2] > 50 if ambe else True, "keyup frames but no decoded audio"
        assert got[-1][2] is False, "no keyup 0 frame after the call"


@pytest.mark.feature("net_usrp")
@pytest.mark.usb_switch
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_usrp_fm_tx(radio, sdr_bench, pytestconfig):
    if not pytestconfig.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    with usrp_session(radio) as (link, peer):
        with radio.vfo_session():
            link.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
            radio.lowest_power()
            before = usrp_view(link)
            with sdr_bench.lease():
                job = sdr_bench.fm_over_start(mhz, 10, audio=True)
                time.sleep(1.0)
                peer.transmit(usrp_client.tone_frames(1000, 3.0))      # then keyup 0
                radio.wait_for(lambda v: v.get("state") != "tx", 2, what="unkeyed by keyup 0")
                time.sleep(1.0)
                # The host stops mid-transmission: the keyup watchdog (300 ms) unkeys
                peer.transmit(usrp_client.tone_frames(1000, 1.0), unkey=False)
                t0 = time.monotonic()
                radio.wait_for(lambda v: v.get("state") != "tx", 2, what="unkeyed by the watchdog")
                watchdog_s = time.monotonic() - t0
                r = sdr_bench.fm_over_result(job)
            after = usrp_view(link)
        print("\n" + fm_measure.describe(r))
        print("usrp:", " ".join(f"{k}={before.get(k)}->{after[k]}" for k in sorted(after)))
        assert int(after["usrp.ptt_downs"]) - int(before["usrp.ptt_downs"]) == 2, after
        assert int(after["usrp.timeouts"]) - int(before["usrp.timeouts"]) == 1, after
        assert int(after["mux.fm_pulls"]) > int(before["mux.fm_pulls"]), "the PWM path never pulled audio"
        assert watchdog_s < 1.0, f"the watchdog took {watchdog_s:.2f} s"
        tones = [t[0] for t in r["tones"]]
        assert any(abs(f - 1000) < 15 for f in tones), f"no 1 kHz tone on air: {tones}"
        rms = r["voice"][0].get("rms") if r["voice"] else None
        assert rms is not None and 500 <= rms <= 2500, f"voice band rms deviation out of range: {r['voice']}"
