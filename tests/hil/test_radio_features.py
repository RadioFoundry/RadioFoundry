# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The everyday radio features (docs/spec/radio-features.md): scan, beeps, battery, settings, and the FM transmit
extras. Radio-only tests never transmit; the radio_tx ones key the radio on its FM VFO at its lowest power (opt-in
--allow-rf); the rf one uses the SDR bench's transmitter (--allow-rf --allow-service-stop).

Everything is read from the view's `features` section (debuglink.md 3.7) and driven by the radio feature operations
(RADIO ops 20-24, debuglink.md 3.8). Settings changed through the SETTING op live in RAM only; each test puts back
the values it changed.

Pass criteria:
- test_features_section: every key present; battery one of ok / low / critical, battery_pct 0-100, battery_mv 5000
  to 9000 (a 2S Li-ion pack; 0 only when the board has no ADC), power_off 0.
- test_settings_round_trip: SETTING scan_hold_s 7 (8 if it was 7) shows in set.scan_hold_s within 1 s, and the old
  value again within 1 s after it is put back.
- test_beep_counts: a BEEP error request counts in beep.played or beep.no_stream (one of them, by 1); when played,
  beep.blocks grows within 1 s (the audio stream ran: on FM this shows whether the I2S clock runs) and beep.busy
  falls back to 0 within 2 s.
- test_vibrate_counts: (CONFIG_VIBRATE builds) a VIBRATE emergency request: on a radio with a motor (vibe.motor=1)
  vibe.played grows by 1 within 1 s and vibe.busy falls back to 0 within 3 s; without one vibe.played stays put.
- test_zone_scan_cancel: (zone of 2+ channels) scan=zone within 1 s; at least 2 different zone positions within 4 s
  unless the scan stopped on a busy channel (scan_phase probe / hold / resume); CANCEL: scan=off and back on the
  starting position within 1 s.
- test_zone_scan_nuisance: (zone of 3+ channels) NUISANCE while scanning: scan_skipped=1 within 1 s; STOP: scan=off,
  scan_skipped=0.
- test_vfo_scan: (FM VFO) SCAN_VFO over the VFO frequency + 0..10 steps: rx_hz takes 3+ values, all inside the range,
  within 4 s; CANCEL: the starting frequency again.
- test_dual_watch: (zone of 2+ channels) dual watch with the next position: zone_pos alternates between exactly the
  two positions; CANCEL: back to the start.
- test_tx_inhibit (radio_tx): set.tx_inhibit=1, PTT: tx_deny=inhibit, the state never shows tx; input.ptt_downs
  counts the press and the debug log has "input: PTT down (host".
- test_tot_warning (radio_tx): tot_s 6, tot_warn_s 3: tot_warning=1 between 2.5 and 4.5 s after key-up, then
  tx_deny=timeout and the state leaves tx between 5.4 and 7.5 s.
- test_roger_beep (radio_tx): roger_beep 1, 1.5 s over: roger=1 is seen after PTT release while still transmitting,
  then the state returns to rx within 1 s.
- test_tone_burst (radio_tx): TONE_BURST 1: state tx and tone_burst=1 within 1 s; TONE_BURST 0: rx within 1 s.
- test_scan_stops_on_dmr_call (rf): the zone holding the DMR test channel scans while the bench sends a 20 superframe
  group call there: the scan holds on that channel (scan_phase hold, rx_hz the test frequency) while the call runs.
- test_vfo_scan_stops_on_signal (rf): the FM VFO scans the DMR test frequency +- 5 steps while the bench sends a 20
  superframe call there: the scan holds (scan_phase hold) with rx_hz the test frequency.
- test_dual_watch_stops_on_call (rf): dual watch between another channel of the zone and the DMR test channel,
  started on the other one: while the bench calls, the scan holds on the test channel's position.
- test_vox_quiet (radio_tx): VOX all, level 5 (the default), tail 500 ms, TOT 5 s as a guard, on the FM VFO at the
  lowest power, nobody talking at the bench: vox_on=1 and the radio does not key (vox_keyed 0, state never tx) in 5 s.
  Then a TOT warning beep (1000 Hz) plays in the speaker: whether it keys VOX through the air is printed (not
  judged; received audio and MONITOR are excluded by design, beeps are not). A voice-triggered key-up needs sound at
  the microphone, which the bench can't make.
- test_battery_save (radio only): rx_save 1 (200 ms asleep, 100 ms listening), rx_save_idle_s 3, the FM VFO on the
  DMR test frequency (quiet): rx_sleeping=1 within 3 + 3 s, and it alternates (asleep and awake both seen within 2 s);
  MONITOR on wakes it (rx_sleeping=0 within 0.5 s and stays 0 for 2 s); the settings are put back.
- test_battery_save_wakes_for_signal (rf): the same, asleep, then the bench sends a 20 superframe call on that
  frequency: the receiver was asleep before, opens (state rx) within 4 s of the bench tool's start (it keys about 2 s
  in), stays open at least 3 s of the 6 s call, and rx_sleeping stays 0 while open.
"""
import contextlib
import threading
import time

import pytest

from odr_debug import as_int

FEATURE_KEYS = ("scan.built", "scan", "scan_phase", "scan_skipped", "tot_warning", "tone_burst", "roger", "vox_keyed",
                "rx_sleeping", "battery", "battery_pct", "battery_mv", "power_off", "beep.built", "set.tot_s",
                "set.tot_warn_s", "set.bcl", "set.roger_beep", "set.tx_inhibit", "set.scan_resume", "set.scan_hold_s",
                "set.scan_delay_ms", "set.scan_dwell_ms", "set.scan_priority", "set.beep_level", "set.key_beep",
                "set.vox", "set.vox_level", "set.vox_tail_ms", "set.rx_save", "set.rx_save_idle_s",
                "set.battery_off_s")
# view value -> SETTING value, for putting settings back
WORDS = {"set.bcl": {"off": 0, "carrier": 1, "wrong_tone": 2}, "set.scan_resume": {"time": 0, "carrier": 1, "stop": 2},
         "set.vox": {"off": 0, "channel": 1, "all": 2}}


def features(radio):
    return radio.link.view(["features"])


def wait_features(radio, pred, timeout=3.0, what="features"):
    return radio.wait_for(pred, timeout, sections=("features",), what=what)


@contextlib.contextmanager
def settings(radio, **values):
    """Set radio settings (RAM) for the block, then put the old values back."""
    before = features(radio)
    old = {}
    for name, value in values.items():
        v = before[f"set.{name}"]
        old[name] = WORDS.get(f"set.{name}", {}).get(v, None)
        if old[name] is None:
            old[name] = int(v)
        radio.link.setting(name, value)
        time.sleep(0.06)        # the radio request rate limit
    try:
        yield
    finally:
        for name, value in old.items():
            radio.link.setting(name, value)
            time.sleep(0.06)


def need_scan(radio):
    if as_int(features(radio).get("scan.built"), 0) != 1:
        pytest.skip("built without CONFIG_SCAN")


def on_channel(radio, minimum):
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    if radio.core().get("vfo") == "1":
        radio.link.vfo(False)
        radio.wait_for(lambda v: v.get("vfo") == "0", what="VFO off")
    size = as_int(radio.core().get("zone_size"), 0)
    if size < minimum:
        pytest.skip(f"the current zone has {size} channels, the test needs {minimum}")
    return size


def scan_off(radio):
    radio.link.scan("cancel")
    wait_features(radio, lambda v: v.get("scan") == "off", what="scan off")


def collect(radio, key, seconds, sections=("core",)):
    """Values of a view key seen in `seconds`, and the last view of the features section."""
    seen, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        v = radio.link.view(list(sections))
        if not seen or seen[-1] != v.get(key):
            seen.append(v.get(key))
        time.sleep(0.03)
    return seen, features(radio)


# ── radio only ────────────────────────────────────────────────────────────

def test_features_section(radio):
    f = features(radio)
    missing = [k for k in FEATURE_KEYS if k not in f]
    assert not missing, f"missing keys in the features section: {missing}"
    assert f["battery"] in ("ok", "low", "critical"), f
    assert 0 <= as_int(f["battery_pct"], -1) <= 100, f
    mv = as_int(f["battery_mv"], -1)
    assert mv == 0 or 5000 <= mv <= 9000, f"battery_mv {mv}"
    assert f["power_off"] == "0", f
    print({k: f[k] for k in ("battery", "battery_pct", "battery_mv", "beep.built", "scan.built")})


def test_settings_round_trip(radio):
    old = features(radio)["set.scan_hold_s"]
    new = 7 if old != "7" else 8
    with settings(radio, scan_hold_s=new):
        wait_features(radio, lambda v: v.get("set.scan_hold_s") == str(new), 1.0, f"set.scan_hold_s={new}")
    wait_features(radio, lambda v: v.get("set.scan_hold_s") == old, 1.0, "set.scan_hold_s put back")


def test_beep_counts(radio):
    f0 = features(radio)
    if f0.get("beep.built") != "1":
        pytest.skip("built without CONFIG_BEEP")
    if as_int(f0.get("set.beep_level"), 0) == 0:
        pytest.skip("beep level 0")
    played0, none0, blocks0 = (as_int(f0.get(k), 0) for k in ("beep.played", "beep.no_stream", "beep.blocks"))
    radio.link.beep("error")
    f = wait_features(radio, lambda v: as_int(v.get("beep.played"), 0) + as_int(v.get("beep.no_stream"), 0)
                      > played0 + none0, 1.0, "the beep counted")
    played = as_int(f["beep.played"], 0) - played0
    no_stream = as_int(f["beep.no_stream"], 0) - none0
    print(f"mode {radio.core().get('mode')}: played +{played}, no_stream +{no_stream}")
    assert played + no_stream == 1, f
    if played:
        f = wait_features(radio, lambda v: as_int(v.get("beep.blocks"), 0) > blocks0, 1.0,
                          "audio blocks (the stream runs)")
        wait_features(radio, lambda v: v.get("beep.busy") == "0", 2.0, "the beep finished")
        print(f"blocks +{as_int(f['beep.blocks'], 0) - blocks0}")


def test_vibrate_counts(radio):
    f0 = features(radio)
    if f0.get("vibe.built") != "1":
        pytest.skip("built without CONFIG_VIBRATE")
    played0 = as_int(f0.get("vibe.played"), 0)
    radio.link.vibrate("emergency")
    if f0.get("vibe.motor") != "1":
        time.sleep(0.5)
        assert as_int(features(radio).get("vibe.played"), 0) == played0
        return
    f = wait_features(radio, lambda v: as_int(v.get("vibe.played"), 0) > played0, 1.0, "the vibration counted")
    assert as_int(f["vibe.played"], 0) == played0 + 1, f
    wait_features(radio, lambda v: v.get("vibe.busy") == "0", 3.0, "the pattern finished")


def test_zone_scan_cancel(radio):
    need_scan(radio)
    on_channel(radio, 2)
    start = radio.core()["zone_pos"]
    with settings(radio, scan_resume=1):
        radio.link.scan("zone")
        try:
            wait_features(radio, lambda v: v.get("scan") == "zone", 1.0, "scan=zone")
            seen, f = collect(radio, "zone_pos", 4.0)
            print("positions:", seen, "phase:", f.get("scan_phase"))
            assert len(set(seen)) >= 2 or f.get("scan_phase") in ("probe", "hold", "resume"), (seen, f)
        finally:
            scan_off(radio)
    radio.wait_for(lambda v: v.get("zone_pos") == start, 1.0, what="back on the starting channel")


def test_zone_scan_nuisance(radio):
    need_scan(radio)
    on_channel(radio, 3)
    radio.link.scan("zone")
    try:
        wait_features(radio, lambda v: v.get("scan") == "zone", 1.0, "scan=zone")
        time.sleep(0.5)
        radio.link.scan("nuisance")
        wait_features(radio, lambda v: v.get("scan_skipped") == "1", 1.0, "scan_skipped=1")
        time.sleep(0.06)
        radio.link.scan("stop")
        f = wait_features(radio, lambda v: v.get("scan") == "off", 1.0, "scan off")
        assert f["scan_skipped"] == "0", f
    finally:
        scan_off(radio)


def test_vfo_scan(radio):
    need_scan(radio)
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    radio.go_home()
    with radio.vfo_session() as hz:
        step = as_int(radio.core().get("vfo_step_hz"), 12500) or 12500
        lo, hi = hz, hz + 10 * step
        radio.link.scan_vfo(lo, hi)
        try:
            wait_features(radio, lambda v: v.get("scan") == "vfo", 1.0, "scan=vfo")
            seen, f = collect(radio, "rx_hz", 4.0)
            vals = {int(x) for x in seen}
            print(f"{len(vals)} frequencies, phase {f.get('scan_phase')}")
            assert all(lo <= x <= hi for x in vals), (sorted(vals), lo, hi)
            assert len(vals) >= 3 or f.get("scan_phase") in ("probe", "hold", "resume"), (sorted(vals), f)
        finally:
            scan_off(radio)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz), 1.0, what="back on the starting frequency")


def test_dual_watch(radio):
    need_scan(radio)
    size = on_channel(radio, 2)
    a = as_int(radio.core()["zone_pos"], 0)
    b = (a + 1) % size
    radio.link.scan("dual", b)
    try:
        wait_features(radio, lambda v: v.get("scan") == "dual", 1.0, "scan=dual")
        seen, f = collect(radio, "zone_pos", 3.0)
        print("positions:", seen, "phase:", f.get("scan_phase"))
        assert set(seen) <= {str(a), str(b)}, seen
        assert len(set(seen)) == 2 or f.get("scan_phase") in ("probe", "hold", "resume"), (seen, f)
    finally:
        scan_off(radio)
    radio.wait_for(lambda v: v.get("zone_pos") == str(a), 1.0, what="back on the first channel")


# ── the radio transmits (FM VFO, lowest power) ─────────────────────────────

@pytest.fixture
def fm_tx(radio, pytestconfig):
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    with radio.vfo_session():
        radio.link.vfo_set(round(pytestconfig.getoption("--fm-mhz") * 1e6))
        radio.wait_for(lambda v: v.get("mode") == "fm", what="FM VFO")
        radio.lowest_power()
        try:
            yield radio
        finally:
            radio.link.ptt(False)
            radio.link.tone_burst(False)
            radio.wait_for(lambda v: v.get("state") != "tx", 3.0, what="TX ended")


@pytest.mark.radio_tx
def test_tx_inhibit(fm_tx):
    with settings(fm_tx, tx_inhibit=1):
        fm_tx.link.log()            # empty the ring
        downs = int(fm_tx.link.view(["link"])["input.ptt_downs"])
        fm_tx.link.ptt(True)
        try:
            seen, _ = collect(fm_tx, "state", 1.0)
            v = fm_tx.core()
        finally:
            fm_tx.link.ptt(False)
        _, log = fm_tx.link.log()
        after = int(fm_tx.link.view(["link"])["input.ptt_downs"])
    assert "tx" not in seen, seen
    assert v.get("tx_deny") == "inhibit", v
    assert after == downs + 1, (downs, after)
    assert "input: PTT down (host" in log, log


@pytest.mark.radio_tx
def test_tot_warning(fm_tx):
    with settings(fm_tx, tot_s=6, tot_warn_s=3):
        t0 = time.monotonic()
        fm_tx.link.ptt(True)
        try:
            fm_tx.wait_for(lambda v: v.get("state") == "tx", 1.0, what="TX")
            wait_features(fm_tx, lambda v: v.get("tot_warning") == "1", 6.0, "tot_warning")
            t_warn = time.monotonic() - t0
            fm_tx.wait_for(lambda v: v.get("state") != "tx", 6.0, what="the time-out")
            t_end = time.monotonic() - t0
            deny = fm_tx.core().get("tx_deny")
        finally:
            fm_tx.link.ptt(False)
    print(f"warning at {t_warn:.1f} s, time-out at {t_end:.1f} s, tx_deny {deny}")
    assert 2.5 <= t_warn <= 4.5, t_warn
    assert 5.4 <= t_end <= 7.5, t_end     # nominally 6 s; the host clock starts just before the PTT request
    assert deny == "timeout", deny


@pytest.mark.radio_tx
def test_roger_beep(fm_tx):
    if "FM_TX_TONES" not in fm_tx.features():
        pytest.skip("built without CONFIG_FM_TX_TONES")
    with settings(fm_tx, roger_beep=1):
        fm_tx.link.ptt(True)
        fm_tx.wait_for(lambda v: v.get("state") == "tx", 1.0, what="TX")
        time.sleep(1.5)
        fm_tx.link.ptt(False)
        rogers = []
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            v = fm_tx.link.view(["core", "features"])
            rogers.append((v.get("state"), v.get("roger")))
            if v.get("state") != "tx":
                break
            time.sleep(0.01)
    print(rogers)
    assert ("tx", "1") in rogers, rogers
    assert rogers[-1][0] != "tx", f"still transmitting 1 s after release: {rogers[-3:]}"


@pytest.mark.radio_tx
def test_tone_burst(fm_tx):
    if "FM_TX_TONES" not in fm_tx.features():
        pytest.skip("built without CONFIG_FM_TX_TONES")
    fm_tx.link.tone_burst(True)
    try:
        fm_tx.wait_for(lambda v: v.get("state") == "tx" and v.get("tone_burst") == "1", 1.0,
                       sections=("core", "features"), what="burst keyed")
        time.sleep(1.0)
    finally:
        fm_tx.link.tone_burst(False)
    fm_tx.wait_for(lambda v: v.get("state") != "tx", 1.0, what="burst over")


# ── the SDR bench transmits ───────────────────────────────────────────────

@pytest.mark.rf
@pytest.mark.bench_service
def test_scan_stops_on_dmr_call(radio, sdr_bench, pytestconfig):
    need_scan(radio)
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    hz = round(freq * 1e6)
    radio.go_home()
    pos = radio.find_channel(hz, "dmr")
    if as_int(radio.core().get("zone_size"), 0) < 2:
        pytest.skip("the DMR test channel's zone has one channel")
    # start somewhere else so the scan has to find it
    radio.link.zone_pos((pos + 1) % as_int(radio.core()["zone_size"], 1))
    time.sleep(0.2)
    held, done, steps = [], threading.Event(), []
    t0 = time.monotonic()

    def watch():
        last = None
        while not done.is_set():
            v = radio.link.view(["core", "features", "dmr"])
            if v.get("scan_phase") == "hold":
                held.append(v.get("rx_hz"))
            now = (v.get("rx_hz"), v.get("scan_phase"), v.get("dmr.state"))
            if now != last:	# the stops and what the radio did at each, for the failure report
                steps.append(f"{time.monotonic() - t0:6.2f} {now[0]} {now[1]} dmr={now[2]}")
                last = now
            time.sleep(0.05)

    with settings(radio, scan_resume=1):
        radio.link.scan("zone")
        t = threading.Thread(target=watch, daemon=True)
        try:
            wait_features(radio, lambda v: v.get("scan") == "zone", 1.0, "scan=zone")
            t.start()
            with sdr_bench.lease():
                sdr_bench.voice_call(freq, cc, tg, superframes=20, calls=1)
        finally:
            done.set()
            t.join(2)
            scan_off(radio)
    f = radio.link.view(["features"])
    # scan_probes: stops probed (a carrier at the end of the dwell); scan_synced: stops where bursts with the channel's
    # color code were heard (the probe then waits up to 1.5 s for the call to play). Probes but no syncs: the modem
    # never delivered a burst there; no probes at all: no carrier seen on the DMR channel (the noise reading)
    print(f"held on {sorted(set(held))} ({len(held)} views); scan_probes={f.get('scan_probes')} "
          f"scan_synced={f.get('scan_synced')}")
    if str(hz) not in held:
        print("\n".join(["steps (s, rx_hz, scan phase, dmr state):"] + steps))
        _, text = radio.log()
        print("\n".join(line for line in text.splitlines() if " dle " in f" {line}"))
    assert str(hz) in held, f"the scan never held on {freq} MHz: {sorted(set(held))}"


# ── scans and battery save against a signal from the SDR bench ───────────

def _bench_call_watch(radio, sdr_bench, pytestconfig, keys=("core", "features")):
    """The bench sends a 20 superframe group call on the DMR test channel while a thread records the view; returns
    (views with their monotonic time, the time the bench started)."""
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    seen, done = [], threading.Event()

    def watch():
        while not done.is_set():
            seen.append((time.monotonic(), radio.link.view(list(keys))))
            time.sleep(0.05)

    t = threading.Thread(target=watch, daemon=True)
    with sdr_bench.lease():
        t.start()
        try:
            t_start = time.monotonic()
            sdr_bench.voice_call(freq, cc, tg, superframes=20, calls=1)
        finally:
            done.set()
            t.join(2)
    return seen, t_start


@pytest.mark.rf
@pytest.mark.bench_service
def test_vfo_scan_stops_on_signal(radio, sdr_bench, pytestconfig):
    need_scan(radio)
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    hz = round(pytestconfig.getoption("--dmr-freq") * 1e6)
    radio.go_home()
    with radio.vfo_session():
        radio.link.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        step = as_int(radio.core().get("vfo_step_hz"), 12500) or 12500
        with settings(radio, scan_resume=1):
            radio.link.scan_vfo(hz - 5 * step, hz + 5 * step)
            try:
                wait_features(radio, lambda v: v.get("scan") == "vfo", 1.0, "scan=vfo")
                seen, _ = _bench_call_watch(radio, sdr_bench, pytestconfig)
            finally:
                scan_off(radio)
    held = [v.get("rx_hz") for _, v in seen if v.get("scan_phase") == "hold"]
    print(f"held on {sorted(set(held))} ({len(held)} views of {len(seen)})")
    assert str(hz) in held, f"the VFO scan never held on {hz}: {sorted(set(held))}"


@pytest.mark.rf
@pytest.mark.bench_service
def test_dual_watch_stops_on_call(radio, sdr_bench, pytestconfig):
    need_scan(radio)
    hz = round(pytestconfig.getoption("--dmr-freq") * 1e6)
    radio.go_home()
    pos = radio.find_channel(hz, "dmr")
    size = as_int(radio.core().get("zone_size"), 0)
    if size < 2:
        pytest.skip("the DMR test channel's zone has one channel")
    other = next((p for p in range(size) if p != pos), None)
    radio.link.zone_pos(other)
    radio.wait_for(lambda v: v.get("zone_pos") == str(other), what="the other channel")
    if radio.core().get("rx_hz") == str(hz):
        pytest.skip("the other channel is on the test frequency too")
    with settings(radio, scan_resume=1):
        radio.link.scan("dual", pos)
        try:
            wait_features(radio, lambda v: v.get("scan") == "dual", 1.0, "scan=dual")
            seen, _ = _bench_call_watch(radio, sdr_bench, pytestconfig)
        finally:
            scan_off(radio)
    positions = {v.get("zone_pos") for _, v in seen}
    held = [v.get("zone_pos") for _, v in seen if v.get("scan_phase") == "hold"]
    print(f"positions {sorted(positions)}, held on {sorted(set(held))} ({len(held)} views)")
    assert positions <= {str(pos), str(other)}, positions
    assert str(pos) in held, f"dual watch never held on position {pos} (the test channel): {sorted(set(held))}"


@contextlib.contextmanager
def _battery_save(radio, pytestconfig):
    if "BATTERY_SAVE" not in radio.features():
        pytest.skip("built without CONFIG_BATTERY_SAVE")
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    hz = round(pytestconfig.getoption("--dmr-freq") * 1e6)
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    with radio.vfo_session():
        radio.link.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        # VOX listens to the microphone and keeps the receiver awake, so it goes off for the test
        with settings(radio, rx_save=1, rx_save_idle_s=3, vox=0):
            v = radio.wait_for(lambda x: x.get("rx_sleeping") == "1", 8.0, interval=0.05, sections=("features",),
                               what="the receiver asleep (rx_sleeping=1)")
            yield v


def test_battery_save(radio, pytestconfig):
    with _battery_save(radio, pytestconfig):
        seen, _ = collect(radio, "rx_sleeping", 2.0, sections=("features",))
        print("rx_sleeping:", seen[:20])
        assert {"0", "1"} <= set(seen), f"the receiver doesn't cycle: {seen}"
        radio.link.monitor(True)
        try:
            radio.wait_for(lambda x: x.get("rx_sleeping") == "0", 0.5, interval=0.03, sections=("features",),
                           what="awake with MONITOR")
            seen, _ = collect(radio, "rx_sleeping", 2.0, sections=("features",))
            assert seen == ["0"], f"asleep again while MONITOR is on: {seen}"
        finally:
            radio.link.monitor(False)


@pytest.mark.rf
@pytest.mark.bench_service
def test_battery_save_wakes_for_signal(radio, sdr_bench, pytestconfig):
    with _battery_save(radio, pytestconfig):
        seen, t_start = _bench_call_watch(radio, sdr_bench, pytestconfig)
    rx = [(t, v) for t, v in seen if v.get("state") == "rx"]
    assert rx, "the receiver never opened for the bench call"
    # bench_tx.py needs about 2 s (port, tuning, settle) before its first burst: measure from the first open
    t_open = rx[0][0]
    asleep_during = [t - t_open for t, v in seen if t_open <= t <= rx[-1][0] and v.get("rx_sleeping") == "1"]
    before = [v.get("rx_sleeping") for t, v in seen if t < t_open]
    print(f"opened {t_open - t_start:.2f} s after the bench started; open for {rx[-1][0] - t_open:.1f} s; "
          f"asleep while open at {asleep_during[:5]}; sleeping before: {before.count('1')} of {len(before)} views")
    assert "1" in before, "the receiver wasn't asleep when the call started"
    assert t_open - t_start <= 4.0, f"opened {t_open - t_start:.2f} s after the bench started (it keys after ~2 s)"
    assert not asleep_during, f"asleep during the call at {asleep_during}"
    assert rx[-1][0] - t_open >= 3.0, f"open for only {rx[-1][0] - t_open:.1f} s of the 6 s call"


@pytest.mark.radio_tx
def test_vox_quiet(fm_tx):
    if "VOX" not in fm_tx.features():
        pytest.skip("built without CONFIG_VOX")
    if fm_tx.view(["features"]).get("vox.mic") == "0":
        pytest.skip("this radio doesn't read its mic level, so VOX can't key")
    with settings(fm_tx, tot_s=5, vox_tail_ms=500, vox_level=5, vox=2):
        try:
            wait_features(fm_tx, lambda x: x.get("vox_on") == "1", 1.0, "VOX armed (vox_on)")
            seen, f = collect(fm_tx, "state", 5.0, sections=("core", "features"))
            keyed = f.get("vox_keyed")
            print(f"quiet: states {seen}, vox_keyed {keyed}")
            assert "tx" not in seen, f"VOX keyed with nobody talking (level 5): {seen}"
            fm_tx.link.beep("tot_warn")
            seen, f = collect(fm_tx, "state", 3.0, sections=("core", "features"))
            print(f"beep in the speaker: states {seen} (VOX {'keyed' if 'tx' in seen else 'did not key'})")
        finally:
            fm_tx.link.setting("vox", 0)
            fm_tx.wait_for(lambda x: x.get("state") != "tx", 3.0, what="VOX off, not transmitting")
