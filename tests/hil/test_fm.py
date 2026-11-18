# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""FM receive. test_noaa_receive, no transmission: the VFO on a NOAA weather radio station (--noaa-mhz, the rig's) opens
the squelch or shows an RSSI above the floor within a few seconds. Depends on reception at the bench, so it is optional:
test_fm_receive_bench is the same check with the bench's carrier. A radio whose bands miss NOAA hears the bench board's
FM transmitter on --fm-mhz instead (--allow-rf, --bench-api): short keys of a 1 kHz tone with no sub-audio tone, the
run's output saying so (a pass there isn't NOAA coverage).

test_fm_receive_bench (rf, bench_service): the VFO on --fm-mhz, the bench board sends a 1 kHz tone for 3 s, and the
squelch opens and closes again after it. test_fm_receive_bench_vhf: the same on --vhf-fm-mhz, at the board's top
transmit level (255; the bench clamps it to its limit): a bench board matched for UHF puts out little on VHF, and at its
default level nothing reached a radio on a dummy load. A radio that doesn't tune the frequency skips (other_board).

test_rx_tone_squelch: CTCSS / DCS decode, from the side NOAA can test (it sends no sub-audio tone): on NOAA with the squelch opening (stepped down from the radio's
level as far as 1 until it does), an RX tone of CTCSS 100.0 Hz and
then DCS D023N must keep the audio muted (tone_ok=0, the state never rx, for 5 s each) while the carrier is there;
with the RX tone back to none the squelch opens again within 10 s. The VFO's RX tone is put back. With the bench
fallback each step is a key of its own: open without an RX tone, muted with each tone, open again. Optional: it needs
NOAA strong enough at the bench, and test_rx_tone_decode_bench covers the same side with the bench's carrier.

test_rx_tone_decode_bench (rf, bench_service): the other side, with the bench board's FM transmitter (--bench-api,
the line protocol's tx op): the VFO on --fm-mhz with an RX tone of CTCSS 100.0 Hz or DCS D023N, the bench sends a
1 kHz tone for 3 s with that sub-audio tone, and the audio opens (state rx, tone_ok=1); then with another one
(CTCSS 131.8 Hz, DCS D025N), and then with none, and the audio stays muted the whole time of each.

test_wide_squelch_opens_like_narrow (optional): at level 1, whatever opens a 12.5 kHz channel opens a 25 kHz one, and
a quiet 25 kHz channel stays shut. NOAA on an antenna, with --allow-rf also the bench's FM at several levels."""
import time

import pytest

from rfoundry_hil import skip
from hil_config import rig

BENCH_FM_S = 3.0        # the bench's 1 kHz tone, under the 10 s key limit with the lead and tail
FALLBACK_PROGRAM = [{"kind": "tone", "hz": 1000, "s": BENCH_FM_S, "level": 0.6}]


def fm_source(radio, pytestconfig, request):
    """Where a test's FM signal without a sub-audio tone comes from: (MHz, None) for NOAA (--noaa-mhz) when the radio
    receives it, else (MHz, the bench) for the bench board's FM transmitter on --fm-mhz, printed as the fallback it is.
    The fallback transmits: it skips (opt_in) without --allow-rf"""
    mhz = rig(pytestconfig, "noaa-mhz")
    if radio.in_band(round(mhz * 1e6)):
        return mhz, None
    if not pytestconfig.getoption("--allow-rf"):
        skip("opt_in", f"NOAA ({mhz:g} MHz) is outside the radio's bands; the bench FM fallback transmits: run with "
                       "--allow-rf")
    fm = rig(pytestconfig, "fm-mhz", "NOAA is outside the radio's bands: the bench FM fallback needs")
    print(f"FALLBACK: NOAA ({mhz:g} MHz) is outside the radio's bands ({radio.core().get('bands')}): the bench "
          f"board's FM on {fm:g} MHz stands in. Not NOAA coverage")
    return fm, request.getfixturevalue("sdr_bench")


def _watch(radio, tx):
    """The radio's (state, tone_ok) pairs while the bench transmits, and the done record"""
    states = set()
    while tx.poll() is None:
        v = radio.core()
        states.add((v.get("state"), v.get("tone_ok")))
        time.sleep(0.1)
    return states, tx.wait(5)


@pytest.mark.feature("fm", "vfo_fm", optional=True)
def test_noaa_receive(radio, pytestconfig, request):
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz, bench = fm_source(radio, pytestconfig, request)
    hz = round(mhz * 1e6)
    radio.skip_out_of_band(hz)
    floor = pytestconfig.getoption("--rssi-floor")
    with radio.vfo_session():
        radio.link.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        best, opened = -999, False
        if bench is None:
            end = time.monotonic() + 10
            while time.monotonic() < end and not opened:
                v = radio.core()
                best = max(best, int(v["rssi_dbm"]))
                opened = v["state"] == "rx"
                time.sleep(0.2)
        else:
            with bench.lease():
                tx = bench.fm_tx_start(mhz, FALLBACK_PROGRAM)
                while tx.poll() is None:
                    v = radio.core()
                    best = max(best, int(v["rssi_dbm"]))
                    opened = opened or v["state"] == "rx"
                    time.sleep(0.2)
                print(f"bench FM: opened {opened}, best RSSI {best} dBm; bench {tx.wait(5)}")
        assert opened or best > floor, (f"no signal on {hz / 1e6:.3f} MHz in 10 s: best RSSI {best} dBm "
                                        f"(floor {floor}); squelch {radio.core()['squelch']}")


WIDE_LEVELS = (128, 180, 255)    # the bench's FM levels: a signal only weak ones leave near the squelch's threshold


def _squelch_to(radio, level):
    for _ in range(12):
        now = int(radio.core()["squelch"])
        if now == level:
            return
        radio.link.squelch_step(1 if now < level else -1)
        time.sleep(0.15)
    raise AssertionError(f"squelch stuck at {radio.core()['squelch']}, wanted {level}")


def _listen(radio, seconds, tx=None):
    """(opened, best RSSI) over `seconds`, or while the bench's transmission `tx` lasts"""
    opened, best = False, -999
    end = time.monotonic() + seconds
    while tx.poll() is None if tx else time.monotonic() < end:
        v = radio.core()
        best = max(best, int(v["rssi_dbm"]))
        opened = opened or v["state"] == "rx"
        time.sleep(0.1)
    return opened, best


@pytest.mark.feature("fm", optional=True)
def test_wide_squelch_opens_like_narrow(radio, pytestconfig, request):
    """The squelch thresholds must suit both filters: at level 1, a signal that opens the 12.5 kHz channel opens the
    25 kHz one, and a quiet 25 kHz channel stays shut. Weak signals are what tell them apart, so a strong one proves
    nothing: NOAA (--noaa-mhz) on an antenna, and with --allow-rf the bench's FM at WIDE_LEVELS. A signal the narrow
    filter doesn't open on is left out; none left: skip."""
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    L = radio.link

    def filter_(wide):
        L.bandwidth(wide)
        radio.wait_for(lambda v: v.get("ch.bw") == ("25k" if wide else "12k5"), sections=("channel",), what="bandwidth")
        time.sleep(0.5)     # the squelch's noise window

    signals = []
    noaa = rig(pytestconfig, "noaa-mhz")
    if radio.in_band(round(noaa * 1e6)):
        signals.append((f"NOAA {noaa:g} MHz", noaa, None))
    bench, fm = None, None
    if pytestconfig.getoption("--allow-rf"):
        fm = rig(pytestconfig, "fm-mhz", "the bench's signal needs")
        bench = request.getfixturevalue("sdr_bench")
        signals += [(f"bench level {p} on {fm:g} MHz", fm, p) for p in WIDE_LEVELS]
    if not signals:
        skip("opt_in", "no NOAA in the radio's bands, and the bench's signal transmits: run with --allow-rf")
    program = [{"kind": "tone", "hz": 1000, "s": BENCH_FM_S, "level": 0.6}]

    def receive(mhz, power):
        if power is None:
            return _listen(radio, 8)
        with bench.lease():
            tx = bench.fm_tx_start(mhz, program, power=power)
            got = _listen(radio, 0, tx)
            tx.wait(5)
        return got

    with radio.vfo_session():
        level0 = int(radio.core()["squelch"])
        problems, compared = [], 0
        try:
            _squelch_to(radio, 1)
            for name, mhz, power in signals:
                L.vfo_set(round(mhz * 1e6))
                radio.wait_for(lambda v: v.get("rx_hz") == str(round(mhz * 1e6)), 3, what="VFO tuned")
                filter_(False)
                narrow, rssi = receive(mhz, power)
                print(f"{name}: 12.5 kHz {'open' if narrow else 'shut'}, best RSSI {rssi} dBm")
                if not narrow:
                    continue
                compared += 1
                filter_(True)
                wide, rssi = receive(mhz, power)
                print(f"{name}: 25 kHz {'open' if wide else 'shut'}, best RSSI {rssi} dBm")
                if not wide:
                    problems.append(f"{name} opens level 1 at 12.5 kHz but not at 25 kHz (best RSSI {rssi} dBm)")
            if bench:
                L.vfo_set(round(fm * 1e6))
                radio.wait_for(lambda v: v.get("rx_hz") == str(round(fm * 1e6)), 3, what="VFO tuned")
                filter_(True)
                with bench.lease():         # nothing of ours on the channel
                    quiet, rssi = _listen(radio, 3)
                print(f"{fm:g} MHz, nothing sent, 25 kHz: {'open' if quiet else 'shut'}, best RSSI {rssi} dBm")
                if quiet:
                    problems.append(f"{fm:g} MHz with nothing sent opened level 1 at 25 kHz ({rssi} dBm)")
        finally:
            filter_(False)
            _squelch_to(radio, level0)
        if not compared:
            skip("bench", "no signal opened the 12.5 kHz channel, so nothing to compare (an antenna, or --allow-rf)")
        assert not problems, "; ".join(problems)


RX_TONES = (("ctcss", 1000, "CTCSS 100.0"), ("dcs", 0o23, "DCS D023N"))


def _tone_squelch_bench(radio, mhz, bench):
    """test_rx_tone_squelch with the bench's FM: a key per step, the RX tone set between them"""
    L = radio.link
    with bench.lease():
        states, done = _watch(radio, bench.fm_tx_start(mhz, FALLBACK_PROGRAM))
        print(f"no RX tone: states {sorted(states)}; bench {done}")
        assert any(st == "rx" for st, _ in states), f"the bench's FM didn't open the squelch: {states}"
        for kind, value, name in RX_TONES:
            L.tone(2, kind, value)
            radio.wait_for(lambda v: v.get("ch.rx_tone") == f"{kind}:{value}", sections=("channel",),
                           what=f"RX tone {name}")
            time.sleep(0.5)          # the tone debounce (8 polls) after the change
            states, done = _watch(radio, bench.fm_tx_start(mhz, FALLBACK_PROGRAM))
            print(f"{name}: states {sorted(states)}; bench {done}")
            assert all(st != "rx" for st, _ in states), f"{name}: the bench's FM (no tone) opened the audio: {states}"
            assert all(t == "0" for _, t in states), f"{name}: a tone decoded on the bench's FM: {states}"
        L.tone(2, "none", 0)
        radio.wait_for(lambda v: v.get("ch.rx_tone") == "none:0", sections=("channel",), what="no RX tone")
        states, done = _watch(radio, bench.fm_tx_start(mhz, FALLBACK_PROGRAM))
        print(f"no RX tone again: states {sorted(states)}; bench {done}")
        assert any(st == "rx" for st, _ in states), f"open again without a tone: {states}"


@pytest.mark.feature("subtone", optional=True)
def test_rx_tone_squelch(radio, pytestconfig, request):
    if "VFO_FM" not in radio.features() or "SUBTONE" not in radio.features():
        skip("not_built", "built without the FM VFO or CTCSS / DCS")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz, bench = fm_source(radio, pytestconfig, request)
    hz = round(mhz * 1e6)
    radio.skip_out_of_band(hz)
    L = radio.link
    with radio.vfo_session():
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        tone0 = radio.view(["channel"])["ch.rx_tone"]
        try:
            L.tone(2, "none", 0)
            radio.wait_for(lambda v: v.get("ch.rx_tone") == "none:0", sections=("channel",), what="no RX tone")
            if bench is not None:
                _tone_squelch_bench(radio, mhz, bench)
                return
            # the squelch stepped down (restore_radio puts it back) until NOAA opens it, but never to 0 (always open)
            while True:
                try:
                    radio.wait_for(lambda v: v.get("state") == "rx", 4, interval=0.2, what="NOAA opens the squelch")
                    break
                except AssertionError:
                    if int(radio.core()["squelch"]) <= 1:
                        skip("bench", f"NOAA on {hz / 1e6:.3f} MHz doesn't open the squelch at level 1: "
                                      "nothing to mute")
                    L.squelch_step(-1)
            print(f"NOAA opens the squelch at level {radio.core()['squelch']}")
            for kind, value, name in RX_TONES:
                L.tone(2, kind, value)
                radio.wait_for(lambda v: v.get("ch.rx_tone") == f"{kind}:{value}", sections=("channel",),
                               what=f"RX tone {name}")
                time.sleep(0.5)          # the tone debounce (8 polls) after the change
                states, rssi, end = set(), [], time.monotonic() + 5
                while time.monotonic() < end:
                    v = radio.core()
                    states.add((v.get("state"), v.get("tone_ok")))
                    rssi.append(int(v["rssi_dbm"]))
                    time.sleep(0.1)
                print(f"{name}: states {sorted(states)}, RSSI {min(rssi)}..{max(rssi)} dBm")
                assert all(st != "rx" for st, _ in states), f"{name}: NOAA (no tone) opened the audio: {states}"
                assert all(t == "0" for _, t in states), f"{name}: a tone decoded on NOAA: {states}"
                L.tone(2, "none", 0)
                radio.wait_for(lambda v: v.get("state") == "rx", 10, interval=0.2, what="open again without a tone")
        finally:
            kind, value = tone0.split(":")
            L.tone(2, kind, int(value))
            radio.wait_for(lambda v: v.get("ch.rx_tone") == tone0, sections=("channel",), what="RX tone restored")



@pytest.mark.feature("fm", "vfo_fm")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.needs_switch("VFO_FM")
def test_fm_receive_bench(radio, sdr_bench, pytestconfig):
    _bench_receive(radio, sdr_bench, pytestconfig, "fm-mhz")


@pytest.mark.feature("fm", "vfo_fm")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.needs_switch("VFO_FM")
def test_fm_receive_bench_vhf(radio, sdr_bench, pytestconfig):
    _bench_receive(radio, sdr_bench, pytestconfig, "vhf-fm-mhz", power=255)


def _bench_receive(radio, sdr_bench, pytestconfig, option, power=None):
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz = rig(pytestconfig, option)
    hz = round(mhz * 1e6)
    program = [{"kind": "tone", "hz": 1000, "s": BENCH_FM_S, "level": 0.6}]
    with radio.vfo_session():
        radio.link.vfo_set(hz)
        try:
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", 3, what="VFO tuned")
        except AssertionError:
            skip("other_board", f"the radio doesn't tune {mhz:.4f} MHz: not one of its bands")
        L = radio.link
        tone0 = radio.view(["channel"])["ch.rx_tone"]
        try:
            if tone0 != "none:0":
                L.tone(2, "none", 0)
                radio.wait_for(lambda v: v.get("ch.rx_tone") == "none:0", sections=("channel",), what="no RX tone")
            floor = max(int(radio.core()["rssi_dbm"]) for _ in range(5))
            states, rssi = set(), []
            with sdr_bench.lease():
                tx = sdr_bench.fm_tx_start(mhz, program, power=power)
                while tx.poll() is None:
                    v = radio.core()
                    states.add(v.get("state"))
                    rssi.append(int(v["rssi_dbm"]))
                    time.sleep(0.1)
                done = tx.wait(5)
            print(f"{mhz:.4f} MHz: states {sorted(states)}, RSSI {floor} dBm before, {min(rssi)}..{max(rssi)} dBm "
                  f"during; bench {done}")
            assert "rx" in states, (f"the bench's carrier on {mhz:.4f} MHz didn't open the squelch {radio.core()['squelch']}: "
                                    f"RSSI {floor} dBm before, at most {max(rssi)} dBm during")
            assert not done.get("cut"), f"the bench cut its transmission: {done}"
            radio.wait_for(lambda v: v.get("state") != "rx", 5, interval=0.1, what="the squelch closes after it")
        finally:
            if tone0 != "none:0":
                k, v = tone0.split(":")
                L.tone(2, k, int(v))
                radio.wait_for(lambda x: x.get("ch.rx_tone") == tone0, sections=("channel",),
                               what="RX tone restored")


@pytest.mark.feature("subtone")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.parametrize("kind, value, sub, wrong", [
    ("ctcss", 1000, {"ctcss": 100.0}, {"ctcss": 131.8}),
    ("dcs", 0o23, {"dcs": "023N"}, {"dcs": "025N"}),
], ids=["ctcss", "dcs"])
@pytest.mark.needs_switch("VFO_FM")
def test_rx_tone_decode_bench(radio, sdr_bench, pytestconfig, kind, value, sub, wrong):
    if "VFO_FM" not in radio.features() or "SUBTONE" not in radio.features():
        skip("not_built", "built without the FM VFO or CTCSS / DCS")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    L = radio.link
    program = [{"kind": "tone", "hz": 1000, "s": BENCH_FM_S, "level": 0.6}]
    with radio.vfo_session():
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        tone0 = radio.view(["channel"])["ch.rx_tone"]
        try:
            L.tone(2, kind, value)
            radio.wait_for(lambda v: v.get("ch.rx_tone") == f"{kind}:{value}", sections=("channel",),
                           what="RX tone set")
            with sdr_bench.lease():
                states, done = _watch(radio, sdr_bench.fm_tx_start(mhz, program, **sub))
                print(f"{sub}: states {sorted(states)}; bench {done}")
                assert ("rx", "1") in states, f"{sub}: the bench's tone didn't open the audio: {states}"
                # the carrier gone, the tone is let go of (else the next carrier opens on the old decode)
                radio.wait_for(lambda v: v.get("state") != "rx" and v.get("tone_ok") == "0", 5, interval=0.1,
                               what="the tone let go of after the carrier")
                for other in (wrong, {}):
                    states, done = _watch(radio, sdr_bench.fm_tx_start(mhz, program, **other))
                    name = other or "no sub-audio tone"
                    print(f"{name}: states {sorted(states)}; bench {done}")
                    assert all(st != "rx" for st, _ in states), f"{name} opened the audio: {states}"
                    assert all(t == "0" for _, t in states), f"{name}: a tone decoded: {states}"
                    assert not done.get("cut"), f"the bench cut its transmission: {done}"
        finally:
            k, v = tone0.split(":")
            L.tone(2, k, int(v))
            radio.wait_for(lambda x: x.get("ch.rx_tone") == tone0, sections=("channel",), what="RX tone restored")
