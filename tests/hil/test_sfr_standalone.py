# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The SFR roles of a codeplug channel, without the hotspot (docs/spec/sfr.md 4). The bench modem is the talker: a
group call on the channel's timeslot with direct mode sync. The tests judge by the radio's own counters (sfr.*,
dmr.*, call.*), not the SDR. They need a channel with the role on --sfr-freq (default --dmr-freq) in the current zone,
set in the codeplug (the tests don't write it), and skip without one. Every setting they change is put back.

test_sfr_repeater_quality (rf, radio_tx, bench_service): a repeater-role channel (Rpt or Rpt Moto) repeats a 4
superframe call. Pass: at least 19 bursts repeated (18 voice and a header); the inbound frames the runner decoded
(sfr.rx_fec) at least 60 of the call's 72, with at most 3 lost and fewer than 4 bit errors a frame on average. The
An AT1846S board with the direct keying gives 0 lost and under 1 bit a frame (gotchas 14.14); the per-frame-place counts (sfr.rx_fec_pos) are printed.

test_sfr_repeater_tx_inhibit (rf, radio_tx, bench_service): with TX inhibit the repeater role lets go of the modem
(dmr.sfr_rpt 0) and a call is not repeated; cleared, the role starts again by itself (dmr.sfr_rpt 1) with no channel
change.

test_sfr_cwid (rf, radio_tx, bench_service): with the CW ID on (setting sfr_cwid 1, 5 minutes) the first ID follows
the first repeated call once the channel has been quiet 2 s. Pass: dmr.sfr_cwid_sent or dmr.sfr_cwid_cut (a radio
name that isn't a callsign: nothing keyed) went up by one. Built without CONFIG_SFR_CWID: skipped.

test_sfr_user_modes (rf, bench_service): a user-role channel (User or User ERDM) hears the talker's direct copy only.
Receive mode 0 (repeater only) drops it (call.rx_sfr_direct counts its voice bursts, nothing plays); mode 1 (first
copy) plays it. The radio doesn't transmit.

test_sfr_chain_rfoundry_user (rf, radio_tx: also --sfr-talker-port, --sfr-listener-port): the whole chain with RadioFoundry radios
only, the thing the bench talker can't show. The --radio-port radio is on a repeater-role channel, the talker and the
listener on user-role channels, all on --sfr-freq. The talker keys TALKER_S seconds at its lowest power with the voice
layer's test signal in place of the mic (RADIO op 33). One call per listener receive mode (--sfr-listen-modes, default
0 and 1). Pass per call, judged by what the listener played, not the RF: one call played (call.rx_calls_played +1),
voice.rx_in at least 90 % of the talker's voice.tx_frames, decoded audio peaking at PEAK_MIN or more (voice.rx_peak)
and at most a tenth of the frames equal to the one before (voice.rx_repeats; the test signal repeats its cycle).
A listener that reads one stale burst for the whole call (the HR_C7000 voice-access flag on the other slot, gotchas
14.15) fails the last two. The repeater's sfr.repeated and the listener's call.rx_sfr_* are printed. TX inhibit is
cleared for the run on the repeater and the talker and put back; the test signal, receive mode and channels too. With
an SDR bench configured, the keying runs inside a bench lease, so shared captures wait for it."""
import contextlib
import time

import pytest

from rfoundry_hil import counter_deltas, skip

RPT_ROLES = (3, 4)  # sfrRole_t: RPT_STANDARD, RPT_MOTOROLA
USER_ROLES = (1, 2)  # USER_STANDARD, USER_ERDM
SRC = 9990302


def _freq(pytestconfig):
    f = pytestconfig.getoption("--sfr-freq") or pytestconfig.getoption("--dmr-freq")
    return f, pytestconfig.getoption("--dmr-cc"), pytestconfig.getoption("--dmr-tg")


def _sfr_channel(radio, hz, roles):
    """Select the first channel of the current zone on hz whose SFR role is in roles; its dmr view, or skip."""
    radio.go_home()
    L = radio.link
    if radio.core().get("vfo") == "1":
        radio._req_until(lambda: L.vfo(False), lambda v: v.get("vfo") == "0", "VFO off")
    size = int(radio.core().get("zone_size") or 0)
    for pos in range(size):
        v = radio._req_until(lambda: L.zone_pos(pos), lambda x: x.get("zone_pos") == str(pos), f"position {pos}")
        if int(v["rx_hz"]) != int(hz) or v.get("mode") != "dmr":
            continue
        d = radio.view(["dmr"])
        if d.get("dmr.sfr_role") is None:
            skip("not_built", "built without the SFR (dmr.sfr_role missing)")
        if int(d["dmr.sfr_role"]) in roles:
            return d
    skip("state", f"no channel with SFR role {roles} on {hz / 1e6:.5f} MHz in zone {radio.core().get('zone_name')!r}")


def _setting(radio, name, value):
    radio.link.setting(name, value)


def _fec(v):
    return [int(x) for x in v.get("sfr.rx_fec", "0/0/0").split("/")]


def _inbound_ts(radio, d):
    """The slot a repeater role listens on: the first of sfr.slots ("1>2"), not dmr.ts, which is the outbound one."""
    slots = radio.view(["sfr"]).get("sfr.slots", "")
    return int(slots.split(">")[0]) if ">" in slots else int(d.get("dmr.ts", 1))


def _call(sdr_bench, radio, freq, cc, tg, ts, superframes=4):
    with sdr_bench.lease():
        sdr_bench.transmit(f"voice --src {SRC} --dst {tg} --superframes {superframes} --family ts{ts}", freq, cc,
                           expect_s=superframes * 0.36 + 3)
    time.sleep(1.5)  # the repeat's tail and the counters


@pytest.fixture
def tx_allowed(radio):
    """TX inhibit off for the test, put back after."""
    old = int(radio.view(["features"]).get("set.tx_inhibit", "0"))
    _setting(radio, "tx_inhibit", 0)
    yield
    _setting(radio, "tx_inhibit", old)


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.feature("sfr")
def test_sfr_repeater_quality(radio, sdr_bench, pytestconfig, tx_allowed):
    freq, cc, tg = _freq(pytestconfig)
    d = _sfr_channel(radio, round(freq * 1e6), RPT_ROLES)
    radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "1", 5, sections=("dmr",), what="repeater role running")
    ts = _inbound_ts(radio, d)
    v0 = radio.view(["sfr"])
    _call(sdr_bench, radio, freq, cc, tg, ts)
    v1 = radio.view(["sfr"])
    f0, f1 = _fec(v0), _fec(v1)
    frames, bits, lost = (f1[i] - f0[i] for i in range(3))
    pos = [int(b) - int(a) for a, b in zip(v0.get("sfr.rx_fec_pos", "0/0/0").split("/"),
                                           v1.get("sfr.rx_fec_pos", "0/0/0").split("/"))]
    rep = int(v1.get("sfr.repeated", 0)) - int(v0.get("sfr.repeated", 0))
    print(f"role {d['dmr.sfr_role']} TS{ts}: repeated {rep}, frames {frames}, bits {bits}, lost {lost}, "
          f"bits by frame place {pos}")
    assert rep >= 19, f"only {rep} bursts repeated (18 voice and a header expected)"
    assert frames >= 60, f"the runner decoded only {frames} of the call's 72 frames"
    assert lost <= 3, f"{lost} inbound frames lost"
    assert bits < 4 * frames, f"{bits} bit errors in {frames} frames"


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.feature("sfr")
def test_sfr_repeater_tx_inhibit(radio, sdr_bench, pytestconfig, tx_allowed):
    freq, cc, tg = _freq(pytestconfig)
    d = _sfr_channel(radio, round(freq * 1e6), RPT_ROLES)
    radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "1", 5, sections=("dmr",), what="repeater role running")
    ts = _inbound_ts(radio, d)
    _setting(radio, "tx_inhibit", 1)
    radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "0", 5, sections=("dmr",), what="repeater role stopped")
    v0 = radio.view(["sfr"])
    _call(sdr_bench, radio, freq, cc, tg, ts, superframes=2)
    v1 = radio.view(["sfr"])
    assert v1.get("sfr.tx_starts") == v0.get("sfr.tx_starts"), "keyed with TX inhibit set"
    _setting(radio, "tx_inhibit", 0)
    radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "1", 10, sections=("dmr",), what="repeater role back")


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.feature("sfr_cwid")
def test_sfr_cwid(radio, sdr_bench, pytestconfig, tx_allowed):
    freq, cc, tg = _freq(pytestconfig)
    d = _sfr_channel(radio, round(freq * 1e6), RPT_ROLES)
    old = int(d.get("dmr.sfr_cwid", 0))
    try:
        _setting(radio, "sfr_cwid", 1)
    except Exception as e:  # the setting is refused without CONFIG_SFR_CWID
        skip("not_built", f"built without the CW ID (CONFIG_SFR_CWID): {e}")
    try:
        radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "1", 5, sections=("dmr",), what="repeater role running")
        d0 = radio.view(["dmr"])
        _call(sdr_bench, radio, freq, cc, tg, _inbound_ts(radio, d), superframes=2)
        # 2 s of quiet, then the ID keys for a few seconds
        d1 = radio.wait_for(lambda x: int(x["dmr.sfr_cwid_sent"]) + int(x["dmr.sfr_cwid_cut"]) >
                            int(d0["dmr.sfr_cwid_sent"]) + int(d0["dmr.sfr_cwid_cut"]) and x.get("dmr.sfr_cw") == "0",
                            20, sections=("dmr",), what="the CW ID")
        print(f"CW ID sent {d0['dmr.sfr_cwid_sent']} -> {d1['dmr.sfr_cwid_sent']}, "
              f"cut {d0['dmr.sfr_cwid_cut']} -> {d1['dmr.sfr_cwid_cut']}")
    finally:
        _setting(radio, "sfr_cwid", old)


@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.feature("sfr")
def test_sfr_user_modes(radio, sdr_bench, pytestconfig):
    freq, cc, tg = _freq(pytestconfig)
    d = _sfr_channel(radio, round(freq * 1e6), USER_ROLES)
    ts = int(d.get("dmr.ts", 1))
    old = int(d.get("dmr.sfr_rx", 0))
    keys = ("call.rx_sfr_direct", "call.rx_voice_bursts", "call.rx_calls")
    try:
        got = {}
        for mode in (0, 1):
            _setting(radio, "sfr_rx", mode)
            v0 = radio.view(["dmr_stats"])
            _call(sdr_bench, radio, freq, cc, tg, ts, superframes=2)
            v1 = radio.view(["dmr_stats"])
            got[mode] = {k: int(v1.get(k, 0)) - int(v0.get(k, 0)) for k in keys}
            print(f"receive mode {mode}: {got[mode]}")
        assert got[0]["call.rx_sfr_direct"] >= 10, f"repeater only: the talker's copy wasn't dropped: {got[0]}"
        assert got[0]["call.rx_voice_bursts"] == 0, f"repeater only: the talker's copy played: {got[0]}"
        assert got[1]["call.rx_sfr_direct"] == 0 and got[1]["call.rx_voice_bursts"] >= 10, \
            f"first copy: the talker's copy didn't play: {got[1]}"
    finally:
        _setting(radio, "sfr_rx", old)


TALKER_S = 4.0
PEAK_MIN = 1000             # as test_dmr_audio_smoke: decoded speech peaks at thousands; stale voice RAM gave 6


def _bench_lease(request):
    """A bench lease when an SDR bench is configured (the radios transmit on a frequency it may be watching), else
    nothing: this test doesn't need the bench."""
    try:  # hil-skip: catches the fixture's skip
        bench = request.getfixturevalue("sdr_bench")
    except pytest.skip.Exception:
        return contextlib.nullcontext()
    return bench.lease()


def _user_radio(port, hz, what):
    """Open the radio on port and put it on a user-role channel on hz; returns (Radio, snapshot, tx_inhibit)."""
    from rfoundry_hil import Radio
    r = Radio(port)
    try:
        if "DMR" not in r.features():
            skip("not_built", f"the {what} is built without DMR")
        snap = r.snapshot()
        _sfr_channel(r, hz, USER_ROLES)
        d = r.view(["dmr", "dmr_stats"])
        if d.get("dmr.codec") != "1":
            skip("not_built", f"the {what} has no vocoder merged (dmr.codec=0)")
        if "voice.rx_repeats" not in d or "voice.tx_test" not in d:
            skip("not_built", f"the {what}'s firmware lacks voice.rx_repeats or the voice test signal (an older build?)")
        inhibit = int(r.view(["features"]).get("set.tx_inhibit", "0"))
        return r, snap, inhibit
    except BaseException:
        r.close()
        raise


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.feature("sfr")
def test_sfr_chain_rfoundry_user(radio, pytestconfig, request, tx_allowed):
    talker_port = pytestconfig.getoption("--sfr-talker-port")
    listener_port = pytestconfig.getoption("--sfr-listener-port")
    if not (talker_port and listener_port):
        skip("opt_in", "needs a talker (--sfr-talker-port) and a listener (--sfr-listener-port) beside the repeater")
    freq, _, _ = _freq(pytestconfig)
    hz = round(freq * 1e6)
    modes = [int(m) for m in pytestconfig.getoption("--sfr-listen-modes").split(",")]
    _sfr_channel(radio, hz, RPT_ROLES)
    radio.wait_for(lambda x: x.get("dmr.sfr_rpt") == "1", 5, sections=("dmr",), what="repeater role running")
    talker, t_snap, t_inhibit = _user_radio(talker_port, hz, "talker")
    try:
        listener, l_snap, _ = _user_radio(listener_port, hz, "listener")
        try:
            l_mode = int(listener.view(["dmr"]).get("dmr.sfr_rx", 0))
            _setting(talker, "tx_inhibit", 0)
            talker.lowest_power()
            talker.link.voice_test(True)
            assert talker.view(["dmr"]).get("voice.tx_test") == "1", "the talker's voice test signal didn't turn on"
            bad = []
            for mode in modes:
                _setting(listener, "sfr_rx", mode)
                bad += _chain_call(request, radio, talker, listener, mode)
            assert not bad, "; ".join(bad)
        finally:
            for step in (lambda: _setting(listener, "sfr_rx", l_mode), lambda: listener.restore(l_snap)):
                with contextlib.suppress(Exception):
                    step()
            listener.close()
    finally:
        for step in (lambda: talker.link.ptt(False), lambda: talker.link.voice_test(False),
                     lambda: _setting(talker, "tx_inhibit", t_inhibit), lambda: talker.restore(t_snap)):
            with contextlib.suppress(Exception):
                step()
        talker.close()


def _chain_call(request, radio, talker, listener, mode):
    """One talker call through the repeater; the failure strings for what the listener played."""
    listener.reset_counters()
    _, t0 = talker.counters()
    _, l0 = listener.counters()
    r0 = radio.view(["sfr"])
    with _bench_lease(request):
        talker.ptt_for(TALKER_S)
        talker.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="talker TX ended")
        time.sleep(2.5)          # the repeat's tail and the listener's playback
    _, t1 = talker.counters()
    _, l1 = listener.counters()
    r1 = radio.view(["sfr"])
    stats = listener.view(["dmr_stats"])
    tx = counter_deltas(t0, t1, ["voice.tx_frames", "voice.tx_repeats", "call.tx_calls"])
    d = counter_deltas(l0, l1, ["call.rx_calls", "call.rx_calls_played", "call.rx_late_entries", "call.rx_voice_bursts",
                                "call.rx_lost_frames", "call.rx_sfr_dup", "call.rx_sfr_direct", "call.rx_sfr_fixed",
                                "voice.rx_in", "voice.rx_repeats", "voice.rx_bad", "voice.rx_bit_errors"])
    peak = int(stats.get("voice.rx_peak", 0))
    rep = int(r1.get("sfr.repeated", 0)) - int(r0.get("sfr.repeated", 0))
    print(f"listener mode {mode}: talker {tx}; repeater repeated {rep}; listener {d}, rx_peak {peak}")
    what = f"listener mode {mode}"
    if tx["call.tx_calls"] != 1 or tx["voice.tx_frames"] < 30 * TALKER_S:
        return [f"{what}: the talker didn't make one call of {TALKER_S:g} s: {tx}"]
    bad = []
    if d["call.rx_calls_played"] != 1:
        bad.append(f"{what}: {d['call.rx_calls_played']} calls played of {d['call.rx_calls']} heard (one expected)")
    if d["voice.rx_in"] < 0.9 * tx["voice.tx_frames"]:
        bad.append(f"{what}: {d['voice.rx_in']} frames into the voice layer of the {tx['voice.tx_frames']} sent")
    if peak < PEAK_MIN:
        bad.append(f"{what}: decoded audio peaks at {peak} < {PEAK_MIN}: silent output (stale or wrong voice bits)")
    if d["voice.rx_repeats"] > d["voice.rx_in"] // 10:
        bad.append(f"{what}: {d['voice.rx_repeats']} of {d['voice.rx_in']} frames repeat the one before: the same "
                   "voice bytes again and again")
    return bad
